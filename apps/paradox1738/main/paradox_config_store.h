#ifndef PARADOX_CONFIG_STORE_H
#define PARADOX_CONFIG_STORE_H

#include "esp_err.h"
#include "paradox_config.h"

typedef enum {
    PARADOX_CONFIG_LOAD_CURRENT = 0,
    PARADOX_CONFIG_LOAD_DEFAULTS,
    PARADOX_CONFIG_LOAD_MIGRATED,
} paradox_config_load_status_t;

esp_err_t paradox_config_store_init(void);
esp_err_t paradox_config_store_load(paradox_config_t *config,
                                    paradox_config_load_status_t *status);
esp_err_t paradox_config_store_save(const paradox_config_t *config);

#ifdef CONFIG_PARADOX_CONFIG_SELFTEST
esp_err_t paradox_config_store_selftest_step(bool *restart_required,
                                             bool *passed);
#endif

#endif
