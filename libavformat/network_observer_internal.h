/* Internal transport observation helpers, LGPL-2.1-or-later. */
#ifndef AVFORMAT_NETWORK_OBSERVER_INTERNAL_H
#define AVFORMAT_NETWORK_OBSERVER_INTERNAL_H
#include "network_observer.h"
int64_t ff_network_observer_begin(void);
void ff_network_observer_end(int64_t start, int64_t syscall_start, int protocol,
                             int direction, int background, const void *id,
                             int fd, int requested, int result);
#endif
