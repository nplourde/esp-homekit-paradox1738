#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "esp_log.h"
#include "paradox1738.h"
#include "paradox_raw_tap.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "paradox1738";

#define BUF_SIZE (1024)
/* The panel sends 4 bytes per event, so a small chunk is plenty. Reading more
 * than one frame at a time keeps the decoder's resync logic exercised. */
#define READ_CHUNK 32
#define READ_TIMEOUT_MS 1000

typedef enum {
    SIMULATOR_IDLE,
    SIMULATOR_REQUESTED,
    SIMULATOR_RUNNING,
    SIMULATOR_COMPLETE,
} simulator_state_t;

const uart_port_t uart_num = UART_NUM_2;

static paradox_decoder_t decoder;
static bool link_ready;
static portMUX_TYPE decoder_stats_mux = portMUX_INITIALIZER_UNLOCKED;

/* Events decoded from one UART read but not handed to the caller yet. A single
 * 32-byte read can contain up to eight frames, and the decoder only queues
 * PARADOX_PENDING_MAX of them, so everything it produced is moved here under
 * the same lock as the push and returned one event per call. Without this the
 * middle frames of a burst are dropped ("incomplete" traffic). */
#ifndef CONFIG_PARADOX_SIMULATOR
#define EVENT_STASH_SIZE 32
static paradox_event_t event_stash[EVENT_STASH_SIZE];
static size_t stash_head;
static size_t stash_count;
#endif
static portMUX_TYPE simulator_mux = portMUX_INITIALIZER_UNLOCKED;
static simulator_state_t simulator_state;
static size_t simulator_position;
static int64_t simulator_next_due_us;

bool paradox1738_simulator_supported(void)
{
#ifdef CONFIG_PARADOX_SIMULATOR
    return false;
#else
    return true;
#endif
}

bool paradox1738_simulator_start(void)
{
    if (!paradox1738_simulator_supported()) {
        return false;
    }

    portENTER_CRITICAL(&simulator_mux);
    bool started = simulator_state == SIMULATOR_IDLE;
    if (started) {
        simulator_state = SIMULATOR_REQUESTED;
        simulator_position = 0;
        simulator_next_due_us = 0;
    }
    portEXIT_CRITICAL(&simulator_mux);
    return started;
}

bool paradox1738_simulator_active(void)
{
    portENTER_CRITICAL(&simulator_mux);
    bool active = simulator_state != SIMULATOR_IDLE;
    portEXIT_CRITICAL(&simulator_mux);
    return active;
}

bool paradox1738_simulator_complete(void)
{
    portENTER_CRITICAL(&simulator_mux);
    bool complete = simulator_state == SIMULATOR_COMPLETE;
    portEXIT_CRITICAL(&simulator_mux);
    return complete;
}

void paradox1738_simulator_cancel(void)
{
    portENTER_CRITICAL(&simulator_mux);
    simulator_state = SIMULATOR_IDLE;
    simulator_position = 0;
    simulator_next_due_us = 0;
    portEXIT_CRITICAL(&simulator_mux);
}

void paradox1738_simulator_finish(void)
{
    portENTER_CRITICAL(&simulator_mux);
    if (simulator_state == SIMULATOR_COMPLETE) {
        simulator_state = SIMULATOR_IDLE;
        simulator_position = 0;
        simulator_next_due_us = 0;
    }
    portEXIT_CRITICAL(&simulator_mux);
}

#ifndef CONFIG_PARADOX_SIMULATOR
static bool simulator_poll(const uint8_t **frame)
{
    bool isolated = false;
    *frame = NULL;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&simulator_mux);
    if (simulator_state != SIMULATOR_IDLE) {
        isolated = true;
        if (simulator_state == SIMULATOR_REQUESTED) {
            simulator_state = SIMULATOR_RUNNING;
            simulator_next_due_us = now;
        }
        if (simulator_state == SIMULATOR_RUNNING &&
            now >= simulator_next_due_us) {
            size_t frame_count = paradox_sim_recorded_frame_count();
            if (simulator_position < frame_count) {
                *frame = paradox_sim_recorded_frames()[simulator_position++];
                simulator_next_due_us = now +
                    (int64_t)paradox_sim_test_interval_ms() * 1000;
                if (simulator_position == frame_count) {
                    simulator_state = SIMULATOR_COMPLETE;
                }
            } else {
                simulator_state = SIMULATOR_COMPLETE;
            }
        }
    }
    portEXIT_CRITICAL(&simulator_mux);
    return isolated;
}
#endif

void paradox1738_stats(paradox_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&decoder_stats_mux);
    *out_stats = decoder.stats;
    portEXIT_CRITICAL(&decoder_stats_mux);
}

void paradox1738_stats_clear(void)
{
    portENTER_CRITICAL(&decoder_stats_mux);
    memset(&decoder.stats, 0, sizeof(decoder.stats));
    portEXIT_CRITICAL(&decoder_stats_mux);
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
    portENTER_CRITICAL(&decoder_stats_mux);
    size_t produced = paradox_decoder_push(&decoder, data, PARADOX_FRAME_LEN,
                                          &evt, 1);
    portEXIT_CRITICAL(&decoder_stats_mux);
    if (produced != 1) {
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

    const uint8_t *simulated_frame = NULL;
    if (simulator_poll(&simulated_frame)) {
        if (simulated_frame == NULL) {
            return false;
        }
        paradox_event_t simulated_event;
        portENTER_CRITICAL(&decoder_stats_mux);
        size_t produced = paradox_decoder_push(
            &decoder, simulated_frame, PARADOX_FRAME_LEN, &simulated_event, 1);
        portEXIT_CRITICAL(&decoder_stats_mux);
        if (produced != 1) {
            ESP_LOGE(TAG, "SIM frame rejected: %02x %02x %02x %02x",
                     simulated_frame[0], simulated_frame[1],
                     simulated_frame[2], simulated_frame[3]);
            return false;
        }
        *out_event = simulated_event;
        ESP_LOGI(TAG, "SIM event: %d %d (%s)", simulated_event.event_id,
                 simulated_event.category_id,
                 paradox_event_name(simulated_event.event_id));
        return true;
    }

    if (!link_ready) {
        return false;
    }

    while (esp_timer_get_time() < deadline) {
        /* Hand back anything left over from a previous read first: one read can
         * decode several frames and only one event may be returned per call. */
        if (stash_count > 0) {
            paradox_event_t evt;
            evt = event_stash[stash_head];
            stash_head = (stash_head + 1) % EVENT_STASH_SIZE;
            stash_count--;
            *out_event = evt;
            ESP_LOGI(TAG, "Recv event: %d %d (%s) raw %02x %02x",
                     evt.event_id, evt.category_id, paradox_event_name(evt.event_id),
                     (uint8_t)(evt.raw >> 8), (uint8_t)evt.raw);
            return true;
        }

        /* The panel sends a couple of frames and then stays quiet for seconds.
         * uart_read_bytes returns early only once it holds the requested count,
         * so asking for a whole buffer parks a 4-byte frame inside the driver
         * for the full timeout - up to a second of dead time between the panel
         * changing and the bridge noticing it. Take what the driver already
         * holds, or wait for the first byte when nothing is buffered. A short
         * read is still normal at 9600 baud, and the decoder is a byte stream,
         * so a frame split across two reads is fine. */
        size_t buffered = 0;
        (void)uart_get_buffered_data_len(uart_num, &buffered);
        size_t want = buffered != 0 ? buffered : 1u;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        int len = uart_read_bytes(uart_num, chunk, want,
                                  READ_TIMEOUT_MS / portTICK_PERIOD_MS);
        if (len <= 0) {
            continue;
        }

        /* Keep a verbatim copy of what came off the wire before the decoder
         * resynchronises and discards bytes. The discarded bytes are where the
         * framing problem is visible, and the decoded-event log can never show
         * them; the web debug endpoints publish this tap as hex over Wi-Fi. */
        paradox_raw_tap_write(chunk, (size_t)len);

        /* Room for every frame this read can contain plus everything the
         * decoder could still have queued from earlier, so the drain below
         * always empties the decoder and its overrun path is never reached. */
        paradox_event_t produced[READ_CHUNK / PARADOX_FRAME_LEN + PARADOX_PENDING_MAX];
        size_t count = 0;

        portENTER_CRITICAL(&decoder_stats_mux);
        count = paradox_decoder_push(&decoder, chunk, (size_t)len, produced,
                                     sizeof(produced) / sizeof(produced[0]));
        /* pop() reads the same state push() writes, so it must run under the
         * same lock. Everything decoded from this read is moved into the stash
         * here; the produced[] capacity above guarantees the decoder is empty
         * afterwards. */
        while (count < sizeof(produced) / sizeof(produced[0]) &&
               paradox_decoder_pop(&decoder, &produced[count])) {
            count++;
        }
        for (size_t i = 0; i < count; i++) {
            size_t tail = (stash_head + stash_count) % EVENT_STASH_SIZE;
            event_stash[tail] = produced[i];
            stash_count++;
        }
        uint32_t framing_errors = decoder.stats.framing_errors;
        uint32_t bytes_discarded = decoder.stats.bytes_discarded;
        portEXIT_CRITICAL(&decoder_stats_mux);

        /* Bytes were partial, or discarded while resynchronising. */
        if (framing_errors != last_framing_errors) {
            last_framing_errors = framing_errors;
            ESP_LOGW(TAG, "Link resynchronised: %lu framing errors, %lu bytes discarded",
                     (unsigned long)framing_errors,
                     (unsigned long)bytes_discarded);
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
#ifndef CONFIG_PARADOX_SIMULATOR
    portENTER_CRITICAL(&decoder_stats_mux);
    stash_head = 0;
    stash_count = 0;
    portEXIT_CRITICAL(&decoder_stats_mux);
#endif

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
    portENTER_CRITICAL(&decoder_stats_mux);
    stash_head = 0;
    stash_count = 0;
    portEXIT_CRITICAL(&decoder_stats_mux);
    ESP_LOGI(TAG, "Paradox 1738 Driver deinit.");
#endif
}
