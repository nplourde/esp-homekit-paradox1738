#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include <hap_fw_upgrade.h>

#include <iot_button.h>

#include <app_wifi.h>
#include <app_hap_setup_payload.h>

#include "paradox1738.h"
#include "paradox_config_store.h"
#include "paradox_state.h"

/* Comment out the below line to disable Firmware Upgrades */
#define CONFIG_FIRMWARE_SERVICE

static const char *TAG = "HAP motionsensor";
static paradox_config_t app_config;

#define MOTIONSENSOR_TASK_PRIORITY  1
#define MOTIONSENSOR_TASK_STACKSIZE 4 * 1024
#define MOTIONSENSOR_TASK_NAME      "hap_motionsensor"

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
/**
 * @brief The factory reset button callback handler.
 */
static void reset_to_factory_handler(void* arg)
{
    hap_reset_to_factory();
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
static int bridge_identify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "Bridge identified");
    return HAP_SUCCESS;
}

static hap_char_t *g_zone_contact_chars[PARADOX_MAX_ZONES];
static hap_char_t *g_zone_fault_chars[PARADOX_MAX_ZONES];
static hap_char_t *g_zone_tampered_chars[PARADOX_MAX_ZONES];
static hap_char_t *g_fire_smoke_char;
static hap_char_t *g_fire_fault_char;
static hap_char_t *g_fire_tampered_char;

static void update_hap_uint8(hap_char_t *hc, uint8_t value)
{
    if (!hc) {
        return;
    }
    hap_val_t val = { .i = value };
    hap_char_update_val(hc, &val);
}

static void update_zone_hap_state(int zone_id, const paradox_zone_state_t *zone)
{
    if (zone_id < 1 || zone_id > PARADOX_MAX_ZONES) {
        return;
    }
    int idx = zone_id - 1;
    update_hap_uint8(g_zone_contact_chars[idx], zone->open ? 1 : 0);
    update_hap_uint8(g_zone_fault_chars[idx], zone->in_alarm ? 1 : 0);
    update_hap_uint8(g_zone_tampered_chars[idx], zone->tampered ? 1 : 0);
}

static void update_fire_hap_state(const paradox_state_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    update_hap_uint8(g_fire_smoke_char, snapshot->fire_alarm ? 1 : 0);
    update_hap_uint8(g_fire_fault_char, snapshot->fire_alarm ? 0 : 0);
    update_hap_uint8(g_fire_tampered_char, snapshot->fire_restored ? 0 : 0);
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
        update_zone_hap_state(zone_id, &snapshot->zones[zone_id - 1]);
    } else if (kind == PARADOX_STATE_CHANGE_FIRE) {
        update_fire_hap_state(snapshot);
    }
}

/*The main thread for handling the Accessory */
static void motionsensor_thread_entry(void *arg)
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

    hap_acc_t *bridge_accessory;

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
        .identify_routine = bridge_identify,
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

    /*
    *    Adding Zone and Fire Accessories
    */

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
        char zone_name[32];
        if (app_config.zones[zone_id - 1].name[0] != '\0') {
            snprintf(zone_name, sizeof(zone_name), "%s", app_config.zones[zone_id - 1].name);
        } else {
            snprintf(zone_name, sizeof(zone_name), "Zone %d", zone_id);
        }

        hap_acc_cfg_t zone_cfg = {
            .name = zone_name,
            .manufacturer = "Paradox",
            .model = "Spectra1738-Zone",
            .serial_num = "000000000000",
            .fw_rev = "0.9.0",
            .hw_rev = NULL,
            .pv = "1.1.0",
            .identify_routine = bridge_identify,
            .cid = HAP_CID_SENSOR,
        };

        char serial_num[16];
        snprintf(serial_num, sizeof(serial_num), "ZONE-%02d", zone_id);
        zone_cfg.serial_num = serial_num;

        hap_acc_t *zone_accessory = hap_acc_create(&zone_cfg);
        if (!zone_accessory) {
            ESP_LOGE(TAG, "Failed to create zone accessory %d", zone_id);
            goto sensor_err;
        }

        hap_serv_t *zone_service = hap_serv_contact_sensor_create(snapshot.zones[zone_id - 1].open ? 1 : 0);
        if (!zone_service) {
            ESP_LOGE(TAG, "Failed to create contact sensor service for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }
        if (hap_serv_add_char(zone_service, hap_char_name_create(zone_name)) != HAP_SUCCESS) {
            ESP_LOGE(TAG, "Failed to add zone name for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }
        if (hap_serv_add_char(zone_service, hap_char_status_fault_create(snapshot.zones[zone_id - 1].in_alarm ? 1 : 0)) != HAP_SUCCESS) {
            ESP_LOGE(TAG, "Failed to add fault char for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }
        if (hap_serv_add_char(zone_service, hap_char_status_tampered_create(snapshot.zones[zone_id - 1].tampered ? 1 : 0)) != HAP_SUCCESS) {
            ESP_LOGE(TAG, "Failed to add tamper char for zone %d", zone_id);
            hap_acc_delete(zone_accessory);
            goto sensor_err;
        }

        hap_acc_add_serv(zone_accessory, zone_service);
        g_zone_contact_chars[zone_id - 1] = hap_serv_get_char_by_uuid(zone_service, HAP_CHAR_UUID_CONTACT_SENSOR_STATE);
        g_zone_fault_chars[zone_id - 1] = hap_serv_get_char_by_uuid(zone_service, HAP_CHAR_UUID_STATUS_FAULT);
        g_zone_tampered_chars[zone_id - 1] = hap_serv_get_char_by_uuid(zone_service, HAP_CHAR_UUID_STATUS_TAMPERED);
        hap_add_bridged_accessory(zone_accessory, hap_get_unique_aid(zone_name));
    }

    hap_acc_cfg_t fire_cfg = {
        .name = "Fire Alarm",
        .manufacturer = "Paradox",
        .model = "Spectra1738-Fire",
        .serial_num = "FIRE-0001",
        .fw_rev = "0.9.0",
        .hw_rev = NULL,
        .pv = "1.1.0",
        .identify_routine = bridge_identify,
        .cid = HAP_CID_SENSOR,
    };
    hap_acc_t *fire_accessory = hap_acc_create(&fire_cfg);
    if (!fire_accessory) {
        ESP_LOGE(TAG, "Failed to create fire accessory");
        goto sensor_err;
    }

    hap_serv_t *fire_service = hap_serv_smoke_sensor_create(snapshot.fire_alarm ? 1 : 0);
    if (!fire_service) {
        ESP_LOGE(TAG, "Failed to create fire service");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    if (hap_serv_add_char(fire_service, hap_char_name_create("Fire Alarm")) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to add fire accessory name");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    if (hap_serv_add_char(fire_service, hap_char_status_fault_create(0)) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to add fire status fault");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    if (hap_serv_add_char(fire_service, hap_char_status_tampered_create(0)) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "Failed to add fire status tampered");
        hap_acc_delete(fire_accessory);
        goto sensor_err;
    }
    hap_acc_add_serv(fire_accessory, fire_service);
    g_fire_smoke_char = hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_SMOKE_DETECTED);
    g_fire_fault_char = hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_STATUS_FAULT);
    g_fire_tampered_char = hap_serv_get_char_by_uuid(fire_service, HAP_CHAR_UUID_STATUS_TAMPERED);
    hap_add_bridged_accessory(fire_accessory, hap_get_unique_aid("Fire Alarm"));

#ifdef CONFIG_FIRMWARE_SERVICE
    hap_serv_t *firmware_service;
    /*  Required for server verification during OTA, PEM format as string  */
    static char server_cert[] = {};
    hap_fw_upgrade_config_t ota_config = {
        .server_cert_pem = server_cert,
    };
    /* Create and add the Firmware Upgrade Service, if enabled.
     * Please refer the FW Upgrade documentation under components/homekit/extras/include/hap_fw_upgrade.h
     * and the top level README for more information.
     */
    firmware_service = hap_serv_fw_upgrade_create(&ota_config);
    if (!firmware_service) {
        ESP_LOGE(TAG, "Failed to create Firmware Upgrade Service");
        goto sensor_err;
    }
    hap_acc_add_serv(bridge_accessory, firmware_service);
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

    /* TODO: Do the actual hardware initialization here */

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
    /* Start Wi-Fi */
    app_wifi_start(portMAX_DELAY);

    for (;;) {
        paradox1738_event_t event;
        if (paradox1738_event_read(&event)) {
            paradox_state_apply_event(&event, esp_timer_get_time() / 1000ULL);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
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
    xTaskCreate(motionsensor_thread_entry, MOTIONSENSOR_TASK_NAME, MOTIONSENSOR_TASK_STACKSIZE, NULL, MOTIONSENSOR_TASK_PRIORITY, NULL);
}
