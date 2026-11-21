#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>

#include <iot_button.h>

#include <app_wifi.h>
#include <app_hap_setup_payload.h>

#include "paradox1738.h"
#include "paradox_config_store.h"
#include "paradox_state.h"
#include "web_server.h"

static const char *TAG = "paradox_hub";
static paradox_config_t app_config;

#define HUB_TASK_PRIORITY  1
#define HUB_TASK_STACKSIZE (8 * 1024)
#define HUB_TASK_NAME      "paradox_hub"

static void confirm_running_firmware(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Cannot identify running firmware partition");
        return;
    }
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read running OTA image state: %s",
                 esp_err_to_name(err));
        return;
    }
#ifdef CONFIG_PARADOX_OTA_ROLLBACK_SELFTEST
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "OTA rollback self-test: rebooting before validation");
        esp_restart();
    }
#endif
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot confirm running firmware: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Running firmware marked valid");
        }
    }
}

/* Reset network credentials if button is pressed for more than 3 seconds and then released */
#define RESET_NETWORK_BUTTON_TIMEOUT        3

/* Reset to factory if button is pressed and held for more than 10 seconds */
#define RESET_TO_FACTORY_BUTTON_TIMEOUT     10

/* The button "Boot" will be used as the Reset button for the example */
#define RESET_GPIO  GPIO_NUM_0
/**
 * @brief The network reset button callback handler.
 * Useful for testing the Wi-Fi re-configuration feature of WAC2
 */
static void reset_network_handler(void* arg)
{
    hap_reset_network();
}
#ifdef CONFIG_PARADOX_WEB_SERVER
static void restart_web_server_after_factory_reset_failure(void)
{
    esp_err_t err = web_server_start(&app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot restart web server after failed factory reset: %s",
                 esp_err_to_name(err));
    }
}
#endif
/**
 * @brief The factory reset button callback handler.
 */
static void reset_to_factory_handler(void* arg)
{
#ifdef CONFIG_PARADOX_WEB_SERVER
    esp_err_t stop_err = web_server_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot stop web server for factory reset: %s",
                 esp_err_to_name(stop_err));
        return;
    }
#endif
    paradox_config_result_t config_result = paradox_config_set_web_auth(
        &app_config, NULL, 0, NULL, 0, false);
    if (config_result != PARADOX_CONFIG_OK) {
        ESP_LOGE(TAG, "Cannot clear web password during factory reset: %d",
                 config_result);
#ifdef CONFIG_PARADOX_WEB_SERVER
        restart_web_server_after_factory_reset_failure();
#endif
        return;
    }
    esp_err_t save_err = paradox_config_store_save(&app_config);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot persist web-password reset: %s",
                 esp_err_to_name(save_err));
#ifdef CONFIG_PARADOX_WEB_SERVER
        restart_web_server_after_factory_reset_failure();
#endif
        return;
    }
    int result = hap_reset_to_factory();
    if (result != HAP_SUCCESS) {
        ESP_LOGE(TAG, "HomeKit factory reset failed: %d", result);
#ifdef CONFIG_PARADOX_WEB_SERVER
        restart_web_server_after_factory_reset_failure();
#endif
    }
}

/**
 * The Reset button  GPIO initialisation function.
 * Same button will be used for resetting Wi-Fi network as well as for reset to factory based on
 * the time for which the button is pressed.
 */
static void reset_key_init(uint32_t key_gpio_pin)
{
    button_handle_t handle = iot_button_create(key_gpio_pin, BUTTON_ACTIVE_LOW);
    iot_button_add_on_release_cb(handle, RESET_NETWORK_BUTTON_TIMEOUT, reset_network_handler, NULL);
    iot_button_add_on_press_cb(handle, RESET_TO_FACTORY_BUTTON_TIMEOUT, reset_to_factory_handler, NULL);
}

/* Mandatory identify routine for the accessory.
 * In a real accessory, something like LED blink should be implemented
 * got visual identification
 */
static int accessory_identify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "HomeKit accessory identified");
    return HAP_SUCCESS;
}

static hap_char_t *g_zone_state_chars[PARADOX_MAX_ZONES];
static hap_char_t *g_zone_fault_chars[PARADOX_MAX_ZONES];
static hap_char_t *g_zone_tampered_chars[PARADOX_MAX_ZONES];
static paradox_hk_service_t g_zone_services[PARADOX_MAX_ZONES];
static hap_char_t *g_fire_smoke_char;
static hap_char_t *g_fire_fault_char;

static int update_hap_uint8(hap_char_t *hc, uint8_t value)
{
    if (!hc) {
        return HAP_FAIL;
    }
    hap_val_t val = { .u = value };
    return hap_char_update_val(hc, &val);
}

static int update_hap_bool(hap_char_t *hc, bool value)
{
    if (!hc) {
        return HAP_FAIL;
    }
    hap_val_t val = { .b = value };
    return hap_char_update_val(hc, &val);
}

static void log_characteristic_update_error(int result, int zone_id,
                                            const char *characteristic)
{
    if (result != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to update %s for zone %d: %d",
                 characteristic, zone_id, result);
    }
}

static bool add_characteristic(hap_serv_t *service, hap_char_t *characteristic)
{
    if (!characteristic) {
        return false;
    }
    if (hap_serv_add_char(service, characteristic) == HAP_SUCCESS) {
        return true;
    }
    hap_char_delete(characteristic);
    return false;
}

static void update_zone_hap_state(int zone_id, const paradox_zone_state_t *zone)
{
    if (!zone || zone_id < 1 || zone_id > PARADOX_MAX_ZONES) {
        return;
    }
    int idx = zone_id - 1;
    if (!g_zone_state_chars[idx]) {
        return;
    }
    int result;
    switch (g_zone_services[idx]) {
    case PARADOX_HK_SERVICE_CONTACT:
        result = update_hap_uint8(g_zone_state_chars[idx], zone->open ? 1 : 0);
        log_characteristic_update_error(result, zone_id, "ContactSensorState");
        break;
    case PARADOX_HK_SERVICE_MOTION:
        result = update_hap_bool(g_zone_state_chars[idx], zone->open);
        log_characteristic_update_error(result, zone_id, "MotionDetected");
        break;
    case PARADOX_HK_SERVICE_SMOKE:
    case PARADOX_HK_SERVICE_LEAK:
        result = update_hap_uint8(g_zone_state_chars[idx],
                                  zone->in_alarm ? 1 : 0);
        log_characteristic_update_error(result, zone_id,
            g_zone_services[idx] == PARADOX_HK_SERVICE_SMOKE
                ? "SmokeDetected" : "LeakDetected");
        break;
    default:
        ESP_LOGE(TAG, "Invalid HomeKit service for zone %d", zone_id);
        return;
    }

    result = update_hap_uint8(g_zone_fault_chars[idx],
                              (zone->in_alarm || zone->bypassed) ? 1 : 0);
    log_characteristic_update_error(result, zone_id, "StatusFault");
    result = update_hap_uint8(g_zone_tampered_chars[idx],
                              zone->tampered ? 1 : 0);
    log_characteristic_update_error(result, zone_id, "StatusTampered");
}

static void update_fire_hap_state(const paradox_state_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    int result = update_hap_uint8(g_fire_smoke_char,
                                  snapshot->fire_alarm ? 1 : 0);
    if (result != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to update SmokeDetected: %d", result);
    }
    result = update_hap_uint8(g_fire_fault_char, snapshot->fire_fault ? 1 : 0);
    if (result != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to update fire StatusFault: %d", result);
    }
}

static void paradox_hap_state_changed(paradox_state_change_kind_t kind,
                                     int zone_id,
                                     const paradox_state_snapshot_t *snapshot,
                                     void *context)
{
    (void)context;
    if (!snapshot) {
        return;
    }
    if (kind == PARADOX_STATE_CHANGE_ZONE) {
        if (zone_id < 1 || zone_id > PARADOX_MAX_ZONES) {
            ESP_LOGE(TAG, "Ignoring invalid zone state notification: %d",
                     zone_id);
            return;
        }
        const paradox_zone_state_t *zone = &snapshot->zones[zone_id - 1];
        ESP_LOGI(TAG,
                 "Zone %d state: open=%d alarm=%d bypassed=%d tampered=%d",
                 zone_id, zone->open, zone->in_alarm, zone->bypassed,
                 zone->tampered);
        update_zone_hap_state(zone_id, zone);
    } else if (kind == PARADOX_STATE_CHANGE_FIRE) {
        ESP_LOGI(TAG, "Fire state: alarm=%d restored=%d fault=%d",
                 snapshot->fire_alarm, snapshot->fire_restored,
                 snapshot->fire_fault);
        update_fire_hap_state(snapshot);
    }
}

static hap_serv_t *create_zone_service(paradox_hk_service_t service,
                                       const paradox_zone_state_t *state,
                                       const char **characteristic_uuid)
{
    if (!state || !characteristic_uuid) {
        return NULL;
    }
    switch (service) {
    case PARADOX_HK_SERVICE_CONTACT:
        *characteristic_uuid = HAP_CHAR_UUID_CONTACT_SENSOR_STATE;
        return hap_serv_contact_sensor_create(state->open ? 1 : 0);
    case PARADOX_HK_SERVICE_MOTION:
        *characteristic_uuid = HAP_CHAR_UUID_MOTION_DETECTED;
        return hap_serv_motion_sensor_create(state->open);
    case PARADOX_HK_SERVICE_SMOKE:
        *characteristic_uuid = HAP_CHAR_UUID_SMOKE_DETECTED;
        return hap_serv_smoke_sensor_create(state->in_alarm ? 1 : 0);
    case PARADOX_HK_SERVICE_LEAK:
        *characteristic_uuid = HAP_CHAR_UUID_LEAK_DETECTED;
        return hap_serv_leak_sensor_create(state->in_alarm ? 1 : 0);
    default:
        return NULL;
    }
}

static unsigned log_hap_database(void)
{
    unsigned accessory_count = 0;
    for (hap_acc_t *accessory = hap_get_first_acc(); accessory;
         accessory = hap_acc_get_next(accessory)) {
        accessory_count++;
        hap_serv_t *info_service = hap_acc_get_serv_by_uuid(
            accessory, HAP_SERV_UUID_ACCESSORY_INFORMATION);
        hap_char_t *name_char = info_service
            ? hap_serv_get_char_by_uuid(info_service, HAP_CHAR_UUID_NAME)
            : NULL;
        const hap_val_t *name_value = name_char
            ? hap_char_get_val(name_char)
            : NULL;
        if (!name_value || !name_value->s) {
            ESP_LOGE(TAG, "Accessory AID %lu has no name characteristic",
                     (unsigned long)hap_acc_get_aid(accessory));
            continue;
        }

        ESP_LOGI(TAG, "HAP accessory AID %lu name \"%s\"",
                 (unsigned long)hap_acc_get_aid(accessory), name_value->s);
        for (hap_serv_t *service = hap_acc_get_first_serv(accessory); service;
             service = hap_serv_get_next(service)) {
            ESP_LOGI(TAG, "HAP service AID %lu UUID %s",
                     (unsigned long)hap_acc_get_aid(accessory),
                     hap_serv_get_type_uuid(service));
        }
    }
    return accessory_count;
}

/* T6.5: read back, as JSON, what the accessory database is advertising.
 *
 * A HomeKit controller cannot be asked over our network: GET /accessories answers
 * 470 unless the requester holds a verified HAP session, and the only controller
 * here is a paired iPhone that will not run a script for us. The values are still
 * readable on the device, and comparing them against GET /api/status is what
 * catches a bridge that updated its state model but not the accessory database -
 * or the other way round.
 *
 * A zone is identified by characteristic-pointer identity: the handles found here
 * are compared with the ones update_zone_hap_state() writes through. Matching on
 * the advertised name would work right up to the point two zones share a name,
 * which the configuration API allows.
 *
 * A characteristic the bridge does not carry on a service prints as -1, which is
 * not a legal value for any characteristic reported here, so "not present" cannot
 * be mistaken for "present and zero". That distinction is what T3.6 asks about for
 * StatusActive and StatusTampered on the fire accessory.
 *
 * Threading: this runs on the web task while the panel task writes these
 * characteristics. Each read lifts one uint8 or bool out of a hap_val_t union, so
 * the worst a race can do is report a value that was already on its way out. The
 * comparison tool re-samples and only reports a disagreement that survives.
 */
#define HOMEKIT_DUMP_BYTES 8192u

static bool hap_read_uint(hap_serv_t *service, const char *uuid, int *value)
{
    hap_char_t *characteristic = hap_serv_get_char_by_uuid(service, uuid);
    if (!characteristic) {
        return false;
    }
    const hap_val_t *val = hap_char_get_val(characteristic);
    if (!val) {
        return false;
    }
    *value = val->u ? 1 : 0;
    return true;
}

/* MotionDetected is written through the bool member of the union, the other state
 * characteristics through the unsigned one. Reading the wrong member of a union
 * means reading whatever the last writer left in the padding. */
static bool hap_read_bool(hap_serv_t *service, const char *uuid, int *value)
{
    hap_char_t *characteristic = hap_serv_get_char_by_uuid(service, uuid);
    if (!characteristic) {
        return false;
    }
    const hap_val_t *val = hap_char_get_val(characteristic);
    if (!val) {
        return false;
    }
    *value = val->b ? 1 : 0;
    return true;
}

static int zone_id_for_state_char(hap_char_t *state_char)
{
    for (int i = 0; i < PARADOX_MAX_ZONES; i++) {
        if (g_zone_state_chars[i] == state_char) {
            return i + 1;
        }
    }
    return 0;
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t used = 0;
    if (out_len == 0) {
        return;
    }
    for (const char *p = in; *p && used + 2 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20) {
            out[used++] = '.';
        } else {
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
}

char *paradox_homekit_dump_json(void)
{
    char *out = malloc(HOMEKIT_DUMP_BYTES);
    if (!out) {
        return NULL;
    }
    char *cursor = out;
    char *end = out + HOMEKIT_DUMP_BYTES;

#define DUMP_APPEND(...)                                                      \
    do {                                                                      \
        int written = snprintf(cursor, (size_t)(end - cursor), __VA_ARGS__);    \
        if (written < 0 || (size_t)written >= (size_t)(end - cursor)) {         \
            free(out);                                                         \
            return NULL;                                                       \
        }                                                                      \
        cursor += written;                                                     \
    } while (0)

    paradox_state_snapshot_t snapshot;
    unsigned long long generation = 0;
    if (paradox_state_snapshot(&snapshot) == PARADOX_STATE_OK) {
        generation = (unsigned long long)snapshot.generation;
    }
    DUMP_APPEND("{\"generation\":%llu,\"accessories\":[", generation);

    bool first = true;
    unsigned sensors = 0;
    for (hap_acc_t *accessory = hap_get_first_acc(); accessory;
         accessory = hap_acc_get_next(accessory)) {
        for (hap_serv_t *service = hap_acc_get_first_serv(accessory); service;
             service = hap_serv_get_next(service)) {
            const char *kind = NULL;
            const char *state_uuid = NULL;
            bool state_is_bool = false;
            hap_char_t *state_char = hap_serv_get_char_by_uuid(
                service, HAP_CHAR_UUID_CONTACT_SENSOR_STATE);
            if (state_char) {
                kind = "contact";
                state_uuid = HAP_CHAR_UUID_CONTACT_SENSOR_STATE;
            } else if ((state_char = hap_serv_get_char_by_uuid(
                            service, HAP_CHAR_UUID_MOTION_DETECTED))) {
                kind = "motion";
                state_uuid = HAP_CHAR_UUID_MOTION_DETECTED;
                state_is_bool = true;
            } else if ((state_char = hap_serv_get_char_by_uuid(
                            service, HAP_CHAR_UUID_SMOKE_DETECTED))) {
                kind = "smoke";
                state_uuid = HAP_CHAR_UUID_SMOKE_DETECTED;
            } else if ((state_char = hap_serv_get_char_by_uuid(
                            service, HAP_CHAR_UUID_LEAK_DETECTED))) {
                kind = "leak";
                state_uuid = HAP_CHAR_UUID_LEAK_DETECTED;
            } else {
                /* Protocol Information, Accessory Information, or the bridge's
                 * own services: nothing here mirrors panel state. */
                continue;
            }

            int state_value = -1;
            if (state_is_bool) {
                hap_read_bool(service, state_uuid, &state_value);
            } else {
                hap_read_uint(service, state_uuid, &state_value);
            }
            int fault = -1;
            int tampered = -1;
            int active = -1;
            hap_read_uint(service, HAP_CHAR_UUID_STATUS_FAULT, &fault);
            hap_read_uint(service, HAP_CHAR_UUID_STATUS_TAMPERED, &tampered);
            if (strcmp(kind, "smoke") == 0) {
                hap_read_uint(service, HAP_CHAR_UUID_STATUS_ACTIVE, &active);
            }

            char name[160];
            name[0] = '\0';
            hap_serv_t *info = hap_acc_get_serv_by_uuid(
                accessory, HAP_SERV_UUID_ACCESSORY_INFORMATION);
            hap_char_t *name_char = info
                ? hap_serv_get_char_by_uuid(info, HAP_CHAR_UUID_NAME)
                : NULL;
            const hap_val_t *name_value = name_char
                ? hap_char_get_val(name_char)
                : NULL;
            if (name_value && name_value->s) {
                json_escape(name_value->s, name, sizeof(name));
            }

            const char *service_uuid = hap_serv_get_type_uuid(service);
            DUMP_APPEND("%s{\"aid\":%lu,\"zone\":%d,\"kind\":\"%s\",\"name\":\"%s\","
                        "\"hap_service_uuid\":\"%s\",\"state\":%d,"
                        "\"status_fault\":%d,\"status_tampered\":%d",
                        first ? "" : ",",
                        (unsigned long)hap_acc_get_aid(accessory),
                        zone_id_for_state_char(state_char), kind, name,
                        service_uuid ? service_uuid : "",
                        state_value, fault, tampered);
            if (active >= 0) {
                DUMP_APPEND(",\"status_active\":%d", active);
            }
            DUMP_APPEND("}");
            first = false;
            sensors++;
        }
    }
    DUMP_APPEND("],\"sensors\":%u}", sensors);
#undef DUMP_APPEND
    return out;
}

static void log_heap_sample(void)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    UBaseType_t stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "Heap sample: internal_free=%u internal_min_free=%u hub_stack_min_free=%u",
             (unsigned)heap_caps_get_free_size(internal_caps),
             (unsigned)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned)stack_min_free);
}

static void hub_task_entry(void *arg)
{
    if (!paradox_state_init()) {
        ESP_LOGE(TAG, "Failed to initialize the Paradox state model");
        vTaskDelete(NULL);
        return;
    }

#ifdef CONFIG_PARADOX_CONFIG_SELFTEST
    bool restart_required;
    bool config_test_passed;
    esp_err_t config_test_err = paradox_config_store_selftest_step(
        &restart_required, &config_test_passed);
    if (config_test_err != ESP_OK) {
        ESP_LOGE(TAG, "Config NVS self-test failed: %s",
                 esp_err_to_name(config_test_err));
        vTaskDelete(NULL);
        return;
    }
    if (restart_required) {
        esp_restart();
    }
    if (!config_test_passed) {
        ESP_LOGE(TAG, "Config NVS self-test did not complete");
        vTaskDelete(NULL);
        return;
    }
#endif

    paradox_config_load_status_t config_status;
    esp_err_t config_err = paradox_config_store_load(&app_config, &config_status);
    if (config_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load Paradox config from NVS: %s",
                 esp_err_to_name(config_err));
        vTaskDelete(NULL);
        return;
    }
    if (config_status == PARADOX_CONFIG_LOAD_DEFAULTS) {
        ESP_LOGI(TAG, "Using default Paradox config");
    } else if (config_status == PARADOX_CONFIG_LOAD_MIGRATED) {
        ESP_LOGI(TAG, "Migrated Paradox config to schema %u",
                 (unsigned)PARADOX_CONFIG_SCHEMA_VERSION);
    } else {
        ESP_LOGI(TAG, "Loaded Paradox config from NVS");
    }

    /*
    *    Adding Bridge Accessory
    */

    hap_acc_t *bridge_accessory = NULL;
    unsigned registered_zones = 0;

    // Initialize the HAP core
    hap_init(HAP_TRANSPORT_WIFI);

    hap_acc_cfg_t bridge_cfg = {
        .name = "Esp-Bridge",
        .manufacturer = "Espressif",
        .model = "EspBridge01",
        .serial_num = "001122334456",
        .fw_rev = "0.9.0",
        .hw_rev = NULL,
        .pv = "1.1.0",
        .identify_routine = accessory_identify,
        .cid = HAP_CID_BRIDGE,
    };

    //Create accessory object
    bridge_accessory = hap_acc_create(&bridge_cfg);    

    if (!bridge_accessory)
    {
        ESP_LOGE(TAG, "Failed to create bridge accessory");
        goto sensor_err;
    }

    // Add a dummy Product Data
    uint8_t product_data[] = {'E','S','P','3','2','H','A','P'};
    hap_acc_add_product_data(bridge_accessory, product_data, sizeof(product_data));

    // Add Wi-Fi Transport service required for HAP Spec R16
    hap_acc_add_wifi_transport_service(bridge_accessory, 0);

    // Add the Accessory to the HomeKit Database
    hap_add_accessory(bridge_accessory);

    /* Register all configured zones with stable IDs independent of names. */

    paradox_state_snapshot_t snapshot;
    if (paradox_state_snapshot(&snapshot) != PARADOX_STATE_OK) {
        ESP_LOGE(TAG, "Failed to read initial Paradox state snapshot");
        goto sensor_err;
    }
    if (paradox_state_set_observer(paradox_hap_state_changed, NULL) != PARADOX_STATE_OK) {
        ESP_LOGE(TAG, "Failed to register HomeKit state observer");
        goto sensor_err;
    }

    for (int zone_id = 1; zone_id <= PARADOX_MAX_ZONES; zone_id++) {
        const paradox_zone_config_t *zone_config =
            &app_config.zones[zone_id - 1];
        if (!zone_config->enabled) {
            continue;
        }

        char zone_name[PARADOX_CONFIG_ZONE_NAME_SIZE];
        int name_length = snprintf(zone_name, sizeof(zone_name), "%s",
                                   zone_config->name);
        if (name_length < 0 || (size_t)name_length >= sizeof(zone_name)) {
            ESP_LOGE(TAG, "Invalid configured name for zone %d", zone_id);
            goto sensor_err;
        }
        if (zone_name[0] == '\0') {
            name_length = snprintf(zone_name, sizeof(zone_name),
                                   "Zone %d", zone_id);
            if (name_length < 0 || (size_t)name_length >= sizeof(zone_name)) {
                ESP_LOGE(TAG, "Failed to format name for zone %d", zone_id);
                goto sensor_err;
            }
        }

        char serial_num[16];
        name_length = snprintf(serial_num, sizeof(serial_num), "ZONE-%02d",
                               zone_id);
        if (name_length < 0 || (size_t)name_length >= sizeof(serial_num)) {
            ESP_LOGE(TAG, "Failed to format serial number for zone %d",
                     zone_id);
            goto sensor_err;
        }

        hap_acc_cfg_t zone_cfg = {
            .name = zone_name,
            .manufacturer = "Paradox",
            .model = "Spectra1738-Zone",
            .serial_num = serial_num,
            .fw_rev = "0.9.0",
            .hw_rev = NULL,
            .pv = "1.1.0",
            .identify_routine = accessory_identify,
            .cid = HAP_CID_SENSOR,
        };

        hap_acc_t *zone_accessory = hap_acc_create(&zone_cfg);
        if (!zone_accessory) {
            ESP_LOGE(TAG, "Failed to create zone accessory %d", zone_id);
            goto sensor_err;
        }

        const paradox_zone_state_t *zone_state = &snapshot.zones[zone_id - 1];
        const char *state_char_uuid = NULL;
        hap_serv_t *zone_service = create_zone_service(zone_config->service,
                                                       zone_state,
                                                       &state_char_uuid);
        if (!zone_service) {
            ESP_LOGE(TAG, "Failed to create HomeKit service for zone %d",
                     zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }

        if (!add_characteristic(zone_service, hap_char_name_create(zone_name)) ||
            !add_characteristic(zone_service, hap_char_status_fault_create(
                (zone_state->in_alarm || zone_state->bypassed) ? 1 : 0)) ||
            !add_characteristic(zone_service, hap_char_status_tampered_create(
                zone_state->tampered ? 1 : 0))) {
            ESP_LOGE(TAG, "Failed to add characteristics for zone %d",
                     zone_id);
            hap_serv_delete(zone_service);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }

        if (hap_acc_add_serv(zone_accessory, zone_service) != HAP_SUCCESS) {
            ESP_LOGE(TAG, "Failed to attach service for zone %d", zone_id);
            hap_serv_delete(zone_service);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }

        g_zone_services[zone_id - 1] = zone_config->service;
        g_zone_state_chars[zone_id - 1] =
            hap_serv_get_char_by_uuid(zone_service, state_char_uuid);
        g_zone_fault_chars[zone_id - 1] =
            hap_serv_get_char_by_uuid(zone_service, HAP_CHAR_UUID_STATUS_FAULT);
        g_zone_tampered_chars[zone_id - 1] =
            hap_serv_get_char_by_uuid(zone_service, HAP_CHAR_UUID_STATUS_TAMPERED);
        if (!g_zone_state_chars[zone_id - 1] ||
            !g_zone_fault_chars[zone_id - 1] ||
            !g_zone_tampered_chars[zone_id - 1]) {
            ESP_LOGE(TAG, "Failed to find characteristics for zone %d",
                     zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }

        char stable_id[16];
        int id_length = snprintf(stable_id, sizeof(stable_id),
                                 "paradox-zone-%02d", zone_id);
        if (id_length < 0 || (size_t)id_length >= sizeof(stable_id)) {
            ESP_LOGE(TAG, "Failed to format stable ID for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }
        int aid = hap_get_unique_aid(stable_id);
        if (aid < 0) {
            ESP_LOGE(TAG, "Failed to get stable AID for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }
        hap_add_bridged_accessory(zone_accessory, aid);
        registered_zones++;
    }

    hap_acc_cfg_t fire_cfg = {
        .name = "Fire Alarm",
        .manufacturer = "Paradox",
        .model = "Spectra1738-Fire",
        .serial_num = "FIRE-0001",
        .fw_rev = "0.9.0",
        .hw_rev = NULL,
        .pv = "1.1.0",
        .identify_routine = accessory_identify,
        .cid = HAP_CID_SENSOR,
    };
    hap_acc_t *fire_accessory = hap_acc_create(&fire_cfg);
    if (!fire_accessory) {
        ESP_LOGE(TAG, "Failed to create fire accessory");
        goto sensor_err;
    }

    hap_serv_t *fire_service = hap_serv_smoke_sensor_create(
        snapshot.fire_alarm ? 1 : 0);
    if (!fire_service ||
        !add_characteristic(fire_service, hap_char_name_create("Fire Alarm")) ||
        !add_characteristic(fire_service, hap_char_status_active_create(true)) ||
        !add_characteristic(fire_service, hap_char_status_fault_create(
            snapshot.fire_fault ? 1 : 0)) ||
        !add_characteristic(fire_service, hap_char_status_tampered_create(0))) {
        ESP_LOGE(TAG, "Failed to create fire sensor characteristics");
        if (fire_service) {
            hap_serv_delete(fire_service);
        }
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    if (hap_acc_add_serv(fire_accessory, fire_service) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to attach fire sensor service");
        hap_serv_delete(fire_service);
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    g_fire_smoke_char =
        hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_SMOKE_DETECTED);
    g_fire_fault_char =
        hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_STATUS_FAULT);
    hap_char_t *fire_status_active_char =
        hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_STATUS_ACTIVE);
    if (!g_fire_smoke_char || !g_fire_fault_char || !fire_status_active_char) {
        ESP_LOGE(TAG, "Failed to find fire sensor characteristics");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    int fire_aid = hap_get_unique_aid("paradox-fire");
    if (fire_aid < 0) {
        ESP_LOGE(TAG, "Failed to get stable AID for fire accessory");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    hap_add_bridged_accessory(fire_accessory, fire_aid);
    ESP_LOGI(TAG,
             "HomeKit bridge registered: %u enabled zones, fire accessory",
             registered_zones);
    unsigned accessory_count = log_hap_database();
    if (accessory_count != registered_zones + 2) {
        ESP_LOGE(TAG, "HAP database has %u accessories; expected %u",
                 accessory_count, registered_zones + 2);
    }
#ifdef CONFIG_PARADOX_WEB_SERVER
    /* Registered only once the database is complete, so a request that arrives
     * earlier gets a clear 503 rather than a half-built picture. */
    web_server_set_homekit_dump(paradox_homekit_dump_json);
#endif

    /* Initialize the Paradox1738 Hardware */
#ifdef CONFIG_PARADOX_LINK_SELFTEST
    /* T1.6: proves paradox1738_deinit() links and is safe to call twice. */
    if (!paradox1738_link_selftest()) {
        ESP_LOGE(TAG, "Paradox link self-test failed");
    }
#else
    paradox1738_init();
#endif

    /* Register a common button for reset Wi-Fi network and reset to factory.
     */
    reset_key_init(RESET_GPIO);

    /* For production accessories, the setup code shouldn't be programmed on to
     * the device. Instead, the setup info, derived from the setup code must
     * be used. Use the factory_nvs_gen utility to generate this data and then
     * flash it into the factory NVS partition.
     *
     * By default, the setup ID and setup info will be read from the factory_nvs
     * Flash partition and so, is not required to set here explicitly.
     *
     * However, for testing purpose, this can be overridden by using hap_set_setup_code()
     * and hap_set_setup_id() APIs, as has been done here.
     */
#ifdef CONFIG_EXAMPLE_USE_HARDCODED_SETUP_CODE
    /* Unique Setup code of the format xxx-xx-xxx. Default: 111-22-333 */
    hap_set_setup_code(CONFIG_EXAMPLE_SETUP_CODE);
    /* Unique four character Setup Id. Default: ES32 */
    hap_set_setup_id(CONFIG_EXAMPLE_SETUP_ID);
#ifdef CONFIG_APP_WIFI_USE_WAC_PROVISIONING
    app_hap_setup_payload(CONFIG_EXAMPLE_SETUP_CODE, CONFIG_EXAMPLE_SETUP_ID, true, bridge_cfg.cid);
#else
    app_hap_setup_payload(CONFIG_EXAMPLE_SETUP_CODE, CONFIG_EXAMPLE_SETUP_ID, false, bridge_cfg.cid);
#endif
#endif

    /* Enable Hardware MFi authentication (applicable only for MFi variant of SDK) */
    hap_enable_mfi_auth(HAP_MFI_AUTH_HW);

    /* Initialize Wi-Fi */
    app_wifi_init();

    /* After all the initializations are done, start the HAP core */
    hap_start();
    bool web_server_ready = true;
#ifdef CONFIG_PARADOX_WEB_SERVER
    esp_err_t web_err = web_server_start(&app_config);
    if (web_err != ESP_OK) {
        web_server_ready = false;
        ESP_LOGE(TAG, "Web server unavailable; continuing with HomeKit: %s",
                 esp_err_to_name(web_err));
    }
#endif
    if (web_server_ready) {
        confirm_running_firmware();
    } else {
        ESP_LOGE(TAG, "Leaving OTA image pending verification because "
                      "web server failed");
    }
    /* Start Wi-Fi */
    app_wifi_start(portMAX_DELAY);

    int64_t next_heap_sample_us = esp_timer_get_time() + 30000000LL;
    bool simulator_snapshot_valid = false;
    paradox_state_snapshot_t simulator_snapshot;
    log_heap_sample();
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_heap_sample_us) {
            log_heap_sample();
            next_heap_sample_us = now_us + 30000000LL;
        }

        if (paradox1738_simulator_active() &&
            !simulator_snapshot_valid) {
            paradox_state_result_t result =
                paradox_state_snapshot(&simulator_snapshot);
            if (result == PARADOX_STATE_OK) {
                simulator_snapshot_valid = true;
            } else {
                ESP_LOGE(TAG, "Cannot snapshot state before simulator replay: %d",
                         result);
                paradox1738_simulator_cancel();
            }
        }

        paradox1738_event_t event;
        if (paradox1738_event_read(&event)) {
            uint64_t event_timestamp_ms = esp_timer_get_time() / 1000ULL;
            web_server_record_event(&event, event_timestamp_ms);
            paradox_state_result_t result = paradox_state_apply_event(
                &event, event_timestamp_ms);
            if (result != PARADOX_STATE_OK &&
                result != PARADOX_STATE_NO_CHANGE &&
                result != PARADOX_STATE_UNSUPPORTED_EVENT) {
                ESP_LOGE(TAG, "Failed to apply panel event %u/%u: %d",
                         event.event_id, event.category_id, result);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (paradox1738_simulator_complete()) {
            paradox_state_result_t result = simulator_snapshot_valid
                ? paradox_state_restore(&simulator_snapshot)
                : PARADOX_STATE_INVALID_ARGUMENT;
            if (result != PARADOX_STATE_OK &&
                result != PARADOX_STATE_NO_CHANGE) {
                ESP_LOGE(TAG, "Cannot restore state after simulator replay: %d",
                         result);
            }
            simulator_snapshot_valid = false;
            paradox1738_simulator_finish();
        }
    }

    /* The task ends here. The read/write callbacks will be invoked by the HAP Framework */
    vTaskDelete(NULL);

sensor_err:
    hap_acc_delete(bridge_accessory);
    vTaskDelete(NULL);
}

void app_main()
{
    BaseType_t result = xTaskCreate(hub_task_entry, HUB_TASK_NAME,
                                    HUB_TASK_STACKSIZE, NULL, HUB_TASK_PRIORITY,
                                    NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HomeKit hub task");
    }
}
