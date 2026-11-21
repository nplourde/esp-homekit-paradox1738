/*
 * Same-origin rule for mutating web requests.
 *
 * The web server has no CSRF token: instead every request that changes something
 * has to name this device in its Origin (or Referer) header. The check below is
 * the whole of that defence, so it is kept here as a plain function - one that
 * takes two strings rather than an esp_http_server request - and pinned by
 * test/host/test_same_origin.c.
 */

#include "paradox_same_origin.h"

#include <string.h>

bool paradox_same_origin_url(const char *url, size_t url_size,
                             const char *host)
{
    static const char prefix[] = "http://";
    const size_t prefix_size = sizeof(prefix) - 1u;

    if (url == NULL || host == NULL || url_size == 0 || host[0] == '\0') {
        return false;
    }

    const size_t host_size = strlen(host);

    /*
     * The Origin has to be "http://" followed by the Host header the request was
     * addressed to, and then either end of string or something that starts a
     * path, query or fragment. Checking the character after the host is what
     * stops a page at "http://192.168.4.30.evil.example" from passing as if it
     * were the device itself.
     */
    if (url_size < prefix_size + host_size ||
        memcmp(url, prefix, prefix_size) != 0 ||
        memcmp(url + prefix_size, host, host_size) != 0) {
        return false;
    }

    const size_t position = prefix_size + host_size;
    return position == url_size || url[position] == '/' ||
           url[position] == '?' || url[position] == '#';
}
