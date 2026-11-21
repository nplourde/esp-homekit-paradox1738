#ifndef PARADOX_WEB_SERVER_H
#define PARADOX_WEB_SERVER_H

#include "esp_err.h"
#include "paradox_config.h"
#include "paradox_proto.h"

esp_err_t web_server_start(paradox_config_t *config);
esp_err_t web_server_stop(void);
void web_server_record_event(const paradox_event_t *event,
                             uint64_t timestamp_ms);

/* The HomeKit bridge registers a callback that serialises the values the esp-hap
 * accessory database is advertising right now, so that GET /api/debug/homekit can
 * be compared against GET /api/status (T6.5). A HomeKit controller cannot be used
 * for that comparison: HAP GET /accessories needs a verified controller session.
 * Before the bridge registers the callback the endpoint answers 503. */
typedef char *(*web_homekit_dump_fn_t)(void);
void web_server_set_homekit_dump(web_homekit_dump_fn_t dumper);

#endif
