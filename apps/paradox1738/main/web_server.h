#ifndef PARADOX_WEB_SERVER_H
#define PARADOX_WEB_SERVER_H

#include "esp_err.h"
#include "paradox_config.h"
#include "paradox_proto.h"

esp_err_t web_server_start(paradox_config_t *config);
esp_err_t web_server_stop(void);
void web_server_record_event(const paradox_event_t *event,
                             uint64_t timestamp_ms);

#endif
