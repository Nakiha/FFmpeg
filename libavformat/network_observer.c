/* Experimental transport observation extension, LGPL-2.1-or-later. */
#include <errno.h>
#include "config.h"
#include "libavutil/time.h"
#include "network_observer_internal.h"
#if HAVE_WINSOCK2_H && CONFIG_NETWORK
#include <winsock2.h>
#endif

static AVNetworkObserver callback;
static void *callback_opaque;

void avformat_set_network_observer(AVNetworkObserver observer, void *opaque)
{
    /* Lifecycle contract excludes concurrent setters or live protocol calls. */
    callback_opaque = opaque;
    callback = observer;
}

int64_t ff_network_observer_begin(void)
{
    return callback ? av_gettime_relative() : 0;
}

void ff_network_observer_end(int64_t start, int64_t syscall_start, int protocol,
                             int direction, int background, const void *id,
                             int fd, int requested, int result)
{
    int saved_errno;
    AVNetworkObservation event;
#if HAVE_WINSOCK2_H && CONFIG_NETWORK
    int saved_wsa;
#endif
    if (!start || !callback)
        return;
    saved_errno = errno;
#if HAVE_WINSOCK2_H && CONFIG_NETWORK
    saved_wsa = WSAGetLastError();
#endif
    event = (AVNetworkObservation) {
        .version = AVFORMAT_NETWORK_OBSERVER_VERSION,
        .protocol = protocol, .direction = direction, .background = background,
        .socket_fd = fd, .connection_id = (uint64_t)(uintptr_t)id,
        .requested_bytes = requested, .result = result,
        .started_us = start, .syscall_start_us = syscall_start,
        .completed_us = av_gettime_relative(),
    };
    callback(callback_opaque, &event);
    errno = saved_errno;
#if HAVE_WINSOCK2_H && CONFIG_NETWORK
    WSASetLastError(saved_wsa);
#endif
}
