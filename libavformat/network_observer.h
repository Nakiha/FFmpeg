/* Experimental transport observation extension, LGPL-2.1-or-later. */
#ifndef AVFORMAT_NETWORK_OBSERVER_H
#define AVFORMAT_NETWORK_OBSERVER_H
#include <stdint.h>
#define AVFORMAT_NETWORK_OBSERVER_VERSION 1

enum AVNetworkProtocol { AV_NETWORK_TCP = 1, AV_NETWORK_UDP = 2 };
enum AVNetworkDirection { AV_NETWORK_RECEIVE = 0, AV_NETWORK_SEND = 1 };
typedef struct AVNetworkObservation {
    int version;
    int protocol, direction, background, socket_fd;
    uint64_t connection_id; /* Opaque identity, not a stable ID after close. */
    int requested_bytes;
    int result; /* >=0 socket result, negative AVERROR on failure/readiness failure. */
    int64_t started_us;       /* Monotonic; includes explicit readiness wait. */
    int64_t syscall_start_us; /* 0 if readiness failed before a socket call. */
    int64_t completed_us;     /* Immediately after the socket call, before UDP FIFO. */
} AVNetworkObservation;
typedef void (*AVNetworkObserver)(void *opaque, const AVNetworkObservation *event);

/**
 * Process-wide, opt-in fork extension. Set only before any network I/O starts;
 * clear/replace only after ALL protocol contexts and background threads close.
 * The observer may run concurrently, including on UDP background threads. It
 * must not block, allocate, log, re-enter FFmpeg, or retain the event pointer.
 * It observes socket API boundaries, not kernel/NIC timestamps or peer receipt.
 * TCP/UDP payload calls only: connect/accept/close, TLS, QUIC and other transports
 * are outside this interface. Disabled observation adds no clock reads.
 */
void avformat_set_network_observer(AVNetworkObserver observer, void *opaque);

#endif
