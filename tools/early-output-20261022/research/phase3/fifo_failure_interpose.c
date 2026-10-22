/* Test-only Darwin interposer, never linked into codec/application builds.
 * Fail one chosen FIFO write while decoding a single stream. Tests use the
 * event log to confirm injection happened; no unobserved-fault pass counts. */
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <libavutil/container_fifo.h>
#include <libavutil/error.h>
static atomic_int writes;
static int fail_write(AVContainerFifo *fifo, void *obj, unsigned flags)
{
    int n = atomic_fetch_add(&writes,1)+1;
    int fail_at = getenv("FAIL_FIFO_WRITE") ? atoi(getenv("FAIL_FIFO_WRITE")) : 0;
    if(n==fail_at) {
        fprintf(stderr,"INJECTED_FIFO_ENOMEM write=%d\n",n);
        return AVERROR(ENOMEM);
    }
    /* Darwin excludes references from this interposing image itself. A
     * RTLD_NEXT lookup can resolve the replacement again and recurse. */
    return av_container_fifo_write(fifo,obj,flags);
}
__attribute__((used)) static struct {const void *replacement,*original;}
interpose_fifo __attribute__((section("__DATA,__interpose"))) =
    {(const void *)fail_write,(const void *)av_container_fifo_write};
