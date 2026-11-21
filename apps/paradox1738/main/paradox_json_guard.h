#ifndef PARADOX_JSON_GUARD_H
#define PARADOX_JSON_GUARD_H

#include <stdbool.h>
#include <stddef.h>

/* How deep a request body's containers may nest.
 *
 * Every body this device accepts is a flat object: a zone edit, a password
 * change, a debug flag. The limit is not about what the API needs, it is about
 * what the web task can survive - cJSON recurses once per nesting level, and the
 * httpd task stack is 8 KiB, so a body like [[[[...]]]] from anyone on the network
 * used to overflow that stack and reboot the module. */
#define PARADOX_JSON_MAX_DEPTH 8u

/* Scan a JSON body and report whether it stays within max_depth of nesting,
 * setting *deepest_out (optional) to the deepest level seen.
 *
 * Brackets inside JSON strings are not counted, and a backslash escape hides the
 * next character, so the count tracks what the parser would actually recurse
 * rather than every bracket in the text.
 *
 * This is a bound, not a validator: unbalanced closers, an unterminated string or
 * text that is not JSON at all all return true here, because rejecting those is
 * the parser's job. The one thing the caller can rely on is that a true result
 * means the parser will never recurse deeper than max_depth. */
bool paradox_json_depth_ok(const char *text, size_t length, size_t max_depth,
                           size_t *deepest_out);

#endif /* PARADOX_JSON_GUARD_H */
