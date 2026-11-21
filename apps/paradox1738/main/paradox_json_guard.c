#include "paradox_json_guard.h"

bool paradox_json_depth_ok(const char *text, size_t length, size_t max_depth,
                           size_t *deepest_out)
{
    if (text == NULL) {
        if (deepest_out != NULL) {
            *deepest_out = 0;
        }
        return false;
    }

    size_t depth = 0;
    size_t deepest = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < length; i++) {
        const char byte = text[i];

        if (in_string) {
            /* Inside a string, brackets are data. An escape hides the next
             * character, which is how a literal quote can sit in a zone name
             * without ending the string and making the rest of the body look
             * like structure it is not. */
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            }
            continue;
        }

        switch (byte) {
        case '"':
            in_string = true;
            break;
        case '{':
        case '[':
            depth++;
            if (depth > deepest) {
                deepest = depth;
            }
            if (depth > max_depth) {
                /* Stop at the breach: the caller rejects the body, and scanning
                 * the rest of a hostile body would spend the very time and stack
                 * this guard exists to protect. */
                if (deepest_out != NULL) {
                    *deepest_out = deepest;
                }
                return false;
            }
            break;
        case '}':
        case ']':
            /* More closers than openers is malformed input, and the parser gets
               to say so. Clamping here only keeps the depth counter from going
               negative while reading it. */
            if (depth > 0) {
                depth--;
            }
            break;
        default:
            break;
        }
    }

    if (deepest_out != NULL) {
        *deepest_out = deepest;
    }
    return true;
}
