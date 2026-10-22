/* Linux test interposer, LGPL-2.1-or-later. Never linked into the SDK.
 * Caller range comes from the matching unstripped build's symbol table. This
 * limits injection to H264's private presentation preparation, never decode or
 * generic frame queues. The runner requires exactly one observed injection. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "libavutil/container_fifo.h"

int av_container_fifo_write(AVContainerFifo *fifo, void *obj, unsigned flags)
{
    typedef int (*Write)(AVContainerFifo *, void *, unsigned);
    Write real = (Write)dlsym(RTLD_NEXT, "av_container_fifo_write");
    static atomic_uint eligible;
    Dl_info info;
    const char *lo=getenv("FAIL_PRESENTATION_LO"), *hi=getenv("FAIL_PRESENTATION_HI");
    const char *nth=getenv("FAIL_PRESENTATION_NTH");
    void *caller=__builtin_return_address(0);
    if (lo && hi && nth && dladdr(caller,&info)) {
        uintptr_t offset=(uintptr_t)caller-(uintptr_t)info.dli_fbase;
        if (offset>=strtoull(lo,NULL,16) && offset<strtoull(hi,NULL,16) &&
            atomic_fetch_add_explicit(&eligible,1,memory_order_relaxed)+1==strtoul(nth,NULL,10)) {
            static const char message[]="presentation_allocation_failure_injected=1\n";
            ssize_t ignored=write(STDERR_FILENO,message,sizeof(message)-1);
            (void)ignored;
            return -ENOMEM;
        }
    }
    return real(fifo,obj,flags);
}
