/* Cooperative dispatch budgets, LGPL-2.1-or-later.
 * Linux named budgets use separate open-file-description locks per slot.
 * The kernel releases them on process death; no heartbeat or PID lease guessing.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "async_budget.h"
#include "libavutil/common.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if HAVE_THREADS
struct AVCodecAsyncBudget {
    atomic_int references;
    pthread_mutex_t mutex;
    int slots, cursor, error;
    unsigned char *busy;
    int *fds;
};
static pthread_mutex_t default_mutex = PTHREAD_MUTEX_INITIALIZER;
static AVCodecAsyncBudget *default_budget;
static void budget_destroy(AVCodecAsyncBudget *budget);

#ifdef __linux__
static int checked_open(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return AVERROR(errno);
    if (fstat(fd, &st) < 0) {
        int ret = AVERROR(errno);
        close(fd);
        return ret;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077)) {
        close(fd);
        return AVERROR(EACCES);
    }
    return fd;
}

static int open_named(AVCodecAsyncBudget *b, const char *name)
{
    struct { char magic[8]; uint32_t slots; } header = { "FAVBUD2", b->slots }, actual;
    char path[256];
    int fd, ret = 0, length = strlen(name);
    ssize_t size;
    if (!length || length > 64)
        return AVERROR(EINVAL);
    for (int i = 0; i < length; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_'))
            return AVERROR(EINVAL);
    snprintf(path, sizeof(path), "/dev/shm/ffmpeg-async-%lu-%s.meta", (unsigned long)getuid(), name);
    fd = checked_open(path);
    if (fd < 0)
        return fd;
    /* Lifecycle-only serialization. Never held while decoding or taking slots. */
    if (flock(fd, LOCK_EX) < 0) { ret = AVERROR(errno); goto end; }
    size = pread(fd, &actual, sizeof(actual), 0);
    if (size < 0) { ret = AVERROR(errno); goto end; }
    if (!size) {
        if (pwrite(fd, &header, sizeof(header), 0) != sizeof(header))
            ret = AVERROR(EIO);
    } else if (size != sizeof(actual) || memcmp(&actual, &header, sizeof(header))) {
        ret = AVERROR(EINVAL); /* Never silently join groups with different capacities. */
    }
end:
    close(fd);
    if (ret < 0)
        return ret;
    b->fds = av_malloc_array(b->slots, sizeof(*b->fds));
    if (!b->fds)
        return AVERROR(ENOMEM);
    for (int i = 0; i < b->slots; i++) b->fds[i] = -1;
    for (int i = 0; i < b->slots; i++) {
        snprintf(path, sizeof(path), "/dev/shm/ffmpeg-async-%lu-%s-%d.slot", (unsigned long)getuid(), name, i);
        b->fds[i] = checked_open(path);
        if (b->fds[i] < 0)
            return b->fds[i];
    }
    return 0;
}
#endif

int avcodec_async_budget_create(AVCodecAsyncBudget **out, int slots, const char *name)
{
    AVCodecAsyncBudget *b;
    int ret;
    if (!out || *out || slots < 0 || slots > 256)
        return AVERROR(EINVAL);
    if (!slots) {
        int cpus = av_cpu_count();
        slots = FFMIN(256, FFMAX(1, cpus - FFMAX(1, cpus / 4)));
    }
    b = av_mallocz(sizeof(*b));
    if (!b)
        return AVERROR(ENOMEM);
    b->slots = slots;
    atomic_init(&b->references, 1);
    ret = pthread_mutex_init(&b->mutex, NULL);
    if (ret) { av_free(b); return AVERROR(ret); }
    b->busy = av_calloc(slots, sizeof(*b->busy));
    if (!b->busy) { ret = AVERROR(ENOMEM); goto fail; }
    if (name) {
#ifdef __linux__
        ret = open_named(b, name);
#else
        ret = AVERROR(ENOSYS);
#endif
        if (ret < 0) goto fail;
    }
    *out = b;
    return 0;
fail:
    budget_destroy(b);
    return ret;
}

AVCodecAsyncBudget *ff_async_budget_ref(AVCodecAsyncBudget *b)
{
    atomic_fetch_add_explicit(&b->references, 1, memory_order_relaxed);
    return b;
}

int ff_async_budget_default(AVCodecAsyncBudget **out)
{
    int ret = 0;
    pthread_mutex_lock(&default_mutex);
    if (!default_budget) {
        ret = avcodec_async_budget_create(&default_budget, 0, NULL);
        if (ret >= 0) *out = default_budget;
    } else
        *out = ff_async_budget_ref(default_budget);
    pthread_mutex_unlock(&default_mutex);
    return ret;
}

int ff_async_budget_take(AVCodecAsyncBudget *b)
{
    int ret = AVERROR(EAGAIN);
    pthread_mutex_lock(&b->mutex);
    if (b->error) { ret = b->error; goto end; }
    for (int i = 0; i < b->slots; i++) {
        int slot = b->cursor;
        b->cursor = (b->cursor + 1) % b->slots;
        if (b->busy[slot]) continue;
#ifdef __linux__
        if (b->fds && flock(b->fds[slot], LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) continue;
            ret = AVERROR(errno);
            break;
        }
#endif
        b->busy[slot] = 1;
        ret = slot;
        break;
    }
end:
    pthread_mutex_unlock(&b->mutex);
    return ret;
}

void ff_async_budget_release(AVCodecAsyncBudget *b, int ticket)
{
    pthread_mutex_lock(&b->mutex);
#ifdef __linux__
    if (b->fds) {
        int ret;
        do { ret = flock(b->fds[ticket], LOCK_UN); } while (ret < 0 && errno == EINTR);
        if (ret < 0) {
            /* Closing the descriptor releases its lock. Quarantine the slot;
             * do not issue a second lease while an unlock may have failed. */
            b->error = AVERROR(errno);
            close(b->fds[ticket]);
            b->fds[ticket] = -1;
            pthread_mutex_unlock(&b->mutex);
            return;
        }
    }
#endif
    b->busy[ticket] = 0;
    pthread_mutex_unlock(&b->mutex);
}

int ff_async_budget_slots(const AVCodecAsyncBudget *b) { return b->slots; }

void avcodec_async_budget_free(AVCodecAsyncBudget **pb)
{
    AVCodecAsyncBudget *b;
    int last;
    if (!pb || !(b = *pb)) return;
    *pb = NULL;
    /* The process-default pointer is weak. Serialize its last reference with
     * acquisition, so repeated owner lifetimes and dlclose do not leak it. */
    pthread_mutex_lock(&default_mutex);
    last = atomic_fetch_sub_explicit(&b->references, 1, memory_order_acq_rel) == 1;
    if (last && b == default_budget) default_budget = NULL;
    pthread_mutex_unlock(&default_mutex);
    if (last) budget_destroy(b);
}

static void budget_destroy(AVCodecAsyncBudget *b)
{
#ifdef __linux__
    if (b->fds)
        for (int i = 0; i < b->slots; i++)
            if (b->fds[i] >= 0) close(b->fds[i]);
#endif
    av_free(b->fds); av_free(b->busy);
    pthread_mutex_destroy(&b->mutex);
    av_free(b);
    /* Named files intentionally persist. Unlinking live lock files could split a group. */
}
#else
int avcodec_async_budget_create(AVCodecAsyncBudget **out, int slots, const char *name)
{ return AVERROR(ENOSYS); }
void avcodec_async_budget_free(AVCodecAsyncBudget **out) {}
#endif
