#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "paradox1738.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "paradox1738";

#define BUF_SIZE (1024)

const uart_port_t uart_num = UART_NUM_2;

bool paradox1738_event_read(paradox1738_event_t *out_event)
{
#ifdef CONFIG_PARADOX_SIMULATOR
    /* Replay recorded panel traffic so the rest of the firmware can be run with
     * no alarm panel attached (T0.4). */
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

    out_event->EventId = data[0] >> 2;
    out_event->CategoryId = ((data[0] & 3) << 4) + (data[1] >> 4);
    ESP_LOGI(TAG, "SIM event: %d %d", out_event->EventId, out_event->CategoryId);
    return true;
#else
    // Configure a temporary buffer for the incoming data
    uint8_t data[4];
    size_t buffered_size;

    if (uart_get_buffered_data_len(uart_num, &buffered_size) == ESP_OK && buffered_size > 3)
    {
        // Read data from the UART
        if(uart_read_bytes(uart_num, data, 4, 20 / portTICK_PERIOD_MS))
        {
            out_event->EventId = data[0] >> 2;
            out_event->CategoryId = ((data[0] & 3) << 4) + (data[1] >> 4);
            ESP_LOGI(TAG, "Recv str: %s", (char *) data);
            ESP_LOGI(TAG, "Recv event: %d %d", out_event->EventId, out_event->CategoryId);
            return 1;
        }
    }

    return 0;
#endif
}

/**
 * @brief initialize the paradox1738 lowlevel module
 */
void paradox1738_init(void)
{
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
#endif
}