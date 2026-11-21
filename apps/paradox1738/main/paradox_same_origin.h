#ifndef PARADOX_SAME_ORIGIN_H
#define PARADOX_SAME_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>

/*
 * True when `url` (an Origin or Referer header value, `url_size` bytes long) is
 * addressed at the same device that answered `Host`. `host` is the Host header
 * exactly as it arrived, so a request that named a port has to be matched by an
 * Origin that names the same port.
 */
bool paradox_same_origin_url(const char *url, size_t url_size,
                             const char *host);

#endif /* PARADOX_SAME_ORIGIN_H */
