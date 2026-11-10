#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "esp_log.h"
#include "paradox1738.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "paradox1738";

#define BUF_SIZE (1024)
/* The panel sends 4 bytes per event, so a small chunk is plenty. Reading more
 * than one frame at a time keeps the decoder's resync logic exercised. */
#define READ_CHUNK 32
#define READ_TIMEOUT_MS 1000

const uart_port_t uart_num = UART_NUM_2;

static paradox_decoder_t decoder;
static bool link_ready;

void paradox1738_stats(paradox_stats_t *out_stats)
{
    *out_stats = decoder.stats;
}

void paradox1738_stats_clear(void)
{
    memset(&decoder.stats, 0, sizeof(decoder.stats));
}

bool paradox1738_event_read(paradox1738_event_t *out_event)
{
#ifdef CONFIG_PARADOX_SIMULATOR
    /* Replay recorded panel traffic so the rest of the firmware can be run with
     * no alarm panel attached (T0.4). The frames are fed through the real
     * decoder, so the decode path is exercised too. */
    static const uint8_t (*frames)[4];
    static size_t frame_count;
    static size_t pos;
    static int64_t next_due_us;

    if (!frames) {
        frames = paradox_sim_recorded_frames();
        frame_count = paradox_sim_recorded_frame_count();
        next_due_us = esp_timer_get_time();
    }

    int64_t now = esp_timer_get_time();
    if (now < next_due_us) {
        vTaskDelay(pdMS_TO_TICKS((next_due_us - now) / 1000 + 1));
        now = esp_timer_get_time();
    }
    next_due_us = now + (int64_t)paradox_sim_replay_interval_ms() * 1000;

    const uint8_t *data = frames[pos];
    pos = (pos + 1) % frame_count;

    paradox_event_t evt;
    if (paradox_decoder_push(&decoder, data, PARADOX_FRAME_LEN, &evt, 1) != 1) {
        ESP_LOGE(TAG, "SIM frame rejected: %02x %02x %02x %02x",
                 data[0], data[1], data[2], data[3]);
        return false;
    }
    *out_event = evt;
    ESP_LOGI(TAG, "SIM event: %d %d (%s)", evt.event_id, evt.category_id,
             paradox_event_name(evt.event_id));
    return true;
#else
    uint8_t chunk[READ_CHUNK];
    int64_t deadline = esp_timer_get_time() + (int64_t)READ_TIMEOUT_MS * 1000;
    uint32_t last_framing_errors = 0;

    if (!link_ready) {
        return false;
    }

    while (esp_timer_get_time() < deadline) {
        /* uart_read_bytes returns how much it actually gave us; a short read is
         * normal at 9600 baud and must not be treated as a full frame. */
        int len = uart_read_bytes(uart_num, chunk, sizeof(chunk),
                                  READ_TIMEOUT_MS / portTICK_PERIOD_MS);
        if (len <= 0) {
            continue;
        }

        paradox_decoder_push(&decoder, chunk, (size_t)len, NULL, 0);

        paradox_event_t evt;
        if (paradox_decoder_pop(&decoder, &evt)) {
            *out_event = evt;
            ESP_LOGI(TAG, "Recv event: %d %d (%s) raw %02x %02x",
                     evt.event_id, evt.category_id, paradox_event_name(evt.event_id),
                     (uint8_t)(evt.raw >> 8), (uint8_t)evt.raw);
            return true;
        }

        /* Bytes were partial, or discarded while resynchronising. */
        if (decoder.stats.framing_errors != last_framing_errors) {
            last_framing_errors = decoder.stats.framing_errors;
            ESP_LOGW(TAG, "Link resynchronised: %lu framing errors, %lu bytes discarded",
                     (unsigned long)decoder.stats.framing_errors,
                     (unsigned long)decoder.stats.bytes_discarded);
        }
    }

    return false;
#endif
}

/**
 * @brief initialize the paradox1738 lowlevel module
 */
void paradox1738_init(void)
{
    paradox_decoder_init(&decoder);

#ifdef CONFIG_PARADOX_SIMULATOR
    ESP_LOGW(TAG, "Paradox 1738 simulator enabled: UART2 not initialised.");
#else
    ESP_LOGI(TAG, "Paradox 1738 Driver Init.");

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    link_ready = true;
#endif
}

bool paradox1738_link_selftest(void)
{
    paradox1738_init();
#ifdef CONFIG_PARADOX_SIMULATOR
    /* No UART driver in a simulator build, so only the call sequence is
     * exercised here. */
    paradox1738_deinit();
    paradox1738_deinit();
    paradox1738_init();
    ESP_LOGI(TAG, "LINK SELFTEST: PASS (simulator build, no UART driver)");
    return true;
#else
    paradox_stats_t stats;

    if (!link_ready) {
        ESP_LOGE(TAG, "LINK SELFTEST: init did not bring the link up");
        return false;
    }
    /* Counters are logged as numbers only: the raw panel buffer is never
     * NUL-terminated, so it must not reach a %s conversion (T1.5). */
    paradox1738_stats(&stats);
    ESP_LOGI(TAG, "LINK SELFTEST: up, frames_ok %lu", (unsigned long)stats.frames_ok);

    paradox1738_deinit();
    paradox1738_deinit(); /* second call must be a harmless no-op (T1.6) */
    if (link_ready) {
        ESP_LOGE(TAG, "LINK SELFTEST: link still up after deinit");
        return false;
    }

    /* Leave the link usable for the rest of the firmware. */
    paradox1738_init();
    if (!link_ready) {
        ESP_LOGE(TAG, "LINK SELFTEST: re-init after deinit failed");
        return false;
    }
    ESP_LOGI(TAG, "LINK SELFTEST: PASS (deinit implemented and idempotent)");
    return true;
#endif
}

void paradox1738_deinit(void)
{
#ifdef CONFIG_PARADOX_SIMULATOR
    link_ready = false;
#else
    if (!link_ready) {
        return;
    }
    uart_driver_delete(uart_num);
    link_ready = false;
    ESP_LOGI(TAG, "Paradox 1738 Driver deinit.");
#endif
}
