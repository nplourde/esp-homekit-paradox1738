#include "paradox_config_store.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "paradox_config";
#ifdef CONFIG_PARADOX_CONFIG_SELFTEST
static const char *NVS_NAMESPACE = "pconfig_test";
#else
static const char *NVS_NAMESPACE = "paradox";
#endif
static const char *NVS_KEY = "config";
static bool nvs_initialized;

esp_err_t paradox_config_store_init(void)
{
    if (nvs_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_initialized = true;
    return ESP_OK;
}

esp_err_t paradox_config_store_save(const paradox_config_t *config)
{
    if (paradox_config_validate(config) != PARADOX_CONFIG_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = paradox_config_store_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *blob = malloc(PARADOX_CONFIG_BLOB_MAX_SIZE);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t blob_size = 0;
    if (!paradox_config_encode(config, blob, PARADOX_CONFIG_BLOB_MAX_SIZE,
                               &blob_size)) {
        free(blob);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(blob);
        ESP_LOGE(TAG, "Cannot open config namespace: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot persist config: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t paradox_config_store_load(paradox_config_t *config,
                                    paradox_config_load_status_t *status)
{
    if (config == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = paradox_config_store_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open config namespace: %s", esp_err_to_name(err));
        return err;
    }

    size_t blob_size = 0;
    err = nvs_get_blob(handle, NVS_KEY, NULL, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        paradox_config_defaults(config);
        *status = PARADOX_CONFIG_LOAD_DEFAULTS;
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Cannot read config size: %s", esp_err_to_name(err));
        return err;
    }
    if (blob_size > paradox_config_blob_max_size()) {
        ESP_LOGW(TAG, "Stored config is oversized; using defaults");
        paradox_config_defaults(config);
        *status = PARADOX_CONFIG_LOAD_DEFAULTS;
        nvs_close(handle);
        return ESP_OK;
    }
    if (blob_size == 0) {
        ESP_LOGW(TAG, "Stored config is empty; using defaults");
        paradox_config_defaults(config);
        *status = PARADOX_CONFIG_LOAD_DEFAULTS;
        nvs_close(handle);
        return ESP_OK;
    }

    uint8_t *blob = malloc(blob_size);
    if (blob == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(handle, NVS_KEY, blob, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(blob);
        ESP_LOGW(TAG, "Stored config cannot be read; using defaults: %s",
                 esp_err_to_name(err));
        paradox_config_defaults(config);
        *status = PARADOX_CONFIG_LOAD_DEFAULTS;
        return ESP_OK;
    }

    bool migrated = false;
    bool decoded = paradox_config_decode(blob, blob_size, config, &migrated);
    free(blob);
    if (!decoded) {
        ESP_LOGW(TAG, "Stored config is invalid or has an unsupported schema; using defaults");
        paradox_config_defaults(config);
        *status = PARADOX_CONFIG_LOAD_DEFAULTS;
        return ESP_OK;
    }

    if (migrated) {
        err = paradox_config_store_save(config);
        if (err != ESP_OK) {
            return err;
        }
        *status = PARADOX_CONFIG_LOAD_MIGRATED;
    } else {
        *status = PARADOX_CONFIG_LOAD_CURRENT;
    }
    return ESP_OK;
}

#ifdef CONFIG_PARADOX_CONFIG_SELFTEST
static const char *SELFTEST_PHASE_KEY = "test_phase";
static paradox_config_t selftest_expected_config;

static bool selftest_config_matches(const paradox_config_t *config)
{
    paradox_config_defaults(&selftest_expected_config);
    if (paradox_config_set_zone_name(&selftest_expected_config, 3,
                                     "M2 test zone 3") != PARADOX_CONFIG_OK ||
        paradox_config_set_zone_service(&selftest_expected_config, 7,
                                        PARADOX_HK_SERVICE_MOTION) !=
            PARADOX_CONFIG_OK ||
        paradox_config_set_zone_enabled(&selftest_expected_config, 12, false) !=
            PARADOX_CONFIG_OK) {
        return false;
    }

    if (paradox_config_validate(config) != PARADOX_CONFIG_OK ||
        config->panel_debug_enabled !=
            selftest_expected_config.panel_debug_enabled ||
        memcmp(config->web_auth_hash, selftest_expected_config.web_auth_hash,
               sizeof(config->web_auth_hash)) != 0) {
        return false;
    }
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        if (config->zones[i].enabled != selftest_expected_config.zones[i].enabled ||
            config->zones[i].service != selftest_expected_config.zones[i].service ||
            strcmp(config->zones[i].name,
                   selftest_expected_config.zones[i].name) != 0) {
            return false;
        }
    }
    return true;
}

static bool selftest_config_is_default(const paradox_config_t *config)
{
    paradox_config_defaults(&selftest_expected_config);
    if (paradox_config_validate(config) != PARADOX_CONFIG_OK ||
        config->panel_debug_enabled ||
        memcmp(config->web_auth_hash, selftest_expected_config.web_auth_hash,
               sizeof(config->web_auth_hash)) != 0) {
        return false;
    }
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        if (config->zones[i].enabled != selftest_expected_config.zones[i].enabled ||
            config->zones[i].service != selftest_expected_config.zones[i].service ||
            strcmp(config->zones[i].name,
                   selftest_expected_config.zones[i].name) != 0) {
            return false;
        }
    }
    return true;
}

static esp_err_t selftest_read_phase(uint8_t *phase)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(handle, SELFTEST_PHASE_KEY, phase);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *phase = 0;
        err = ESP_OK;
    }
    nvs_close(handle);
    return err;
}

static esp_err_t selftest_write_phase(uint8_t phase)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, SELFTEST_PHASE_KEY, phase);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t selftest_write_legacy_blob(const paradox_config_t *config)
{
    const size_t blob_size = PARADOX_CONFIG_BLOB_MAX_SIZE -
                             PARADOX_CONFIG_WEB_AUTH_HASH_SIZE;
    uint8_t *blob = malloc(blob_size);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t offset = 0;
    blob[offset++] = 0;
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_config_t *zone = &config->zones[i];
        blob[offset++] = zone->enabled ? 1u : 0u;
        blob[offset++] = (uint8_t)zone->service;
        memcpy(blob + offset, zone->name, sizeof(zone->name));
        offset += sizeof(zone->name);
    }
    blob[offset++] = config->panel_debug_enabled ? 1u : 0u;
    if (offset != blob_size) {
        free(blob);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SELFTEST_PHASE_KEY, 2);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    free(blob);
    return err;
}

static esp_err_t selftest_write_corrupt_blob(void)
{
    const uint8_t corrupt_blob[] = {0xff};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, corrupt_blob, sizeof(corrupt_blob));
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SELFTEST_PHASE_KEY, 3);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t paradox_config_store_selftest_step(bool *restart_required,
                                             bool *passed)
{
    if (restart_required == NULL || passed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *restart_required = false;
    *passed = false;

    esp_err_t err = paradox_config_store_init();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t phase;
    err = selftest_read_phase(&phase);
    if (err != ESP_OK) {
        return err;
    }

    static paradox_config_t config;
    paradox_config_load_status_t load_status;
    switch (phase) {
    case 1:
        err = paradox_config_store_load(&config, &load_status);
        if (err != ESP_OK) {
            return err;
        }
        if (load_status != PARADOX_CONFIG_LOAD_CURRENT ||
            !selftest_config_matches(&config)) {
            return ESP_ERR_INVALID_STATE;
        }
        err = selftest_write_legacy_blob(&config);
        if (err == ESP_OK) {
            *restart_required = true;
        }
        return err;
    case 2:
        err = paradox_config_store_load(&config, &load_status);
        if (err != ESP_OK) {
            return err;
        }
        if (load_status != PARADOX_CONFIG_LOAD_MIGRATED ||
            !selftest_config_matches(&config)) {
            return ESP_ERR_INVALID_STATE;
        }
        err = selftest_write_corrupt_blob();
        if (err == ESP_OK) {
            *restart_required = true;
        }
        return err;
    case 3:
        err = paradox_config_store_load(&config, &load_status);
        if (err != ESP_OK) {
            return err;
        }
        if (load_status != PARADOX_CONFIG_LOAD_DEFAULTS ||
            !selftest_config_is_default(&config)) {
            return ESP_ERR_INVALID_STATE;
        }
        err = paradox_config_store_save(&config);
        if (err == ESP_OK) {
            err = selftest_write_phase(4);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CONFIG SELFTEST: PASS (NVS round-trip, migration, corrupt fallback)");
            *passed = true;
        }
        return err;
    default:
        paradox_config_defaults(&config);
        err = paradox_config_set_zone_name(&config, 3, "M2 test zone 3");
        if (err != PARADOX_CONFIG_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        err = paradox_config_set_zone_service(
            &config, 7, PARADOX_HK_SERVICE_MOTION);
        if (err != PARADOX_CONFIG_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        err = paradox_config_set_zone_enabled(&config, 12, false);
        if (err != PARADOX_CONFIG_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        err = paradox_config_store_save(&config);
        if (err == ESP_OK) {
            err = selftest_write_phase(1);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CONFIG SELFTEST: saved fixture; rebooting for persistence check");
            *restart_required = true;
        }
        return err;
    }
}
#endif
