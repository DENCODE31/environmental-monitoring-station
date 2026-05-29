/* ============================================================
 *   status_led — implementación
 * ============================================================ */
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"

#define LED_GPIO   8       /* WS2812 onboard del ESP32-C6-DevKitC-1 */
#define BLINK_MS   300     /* Periodo de parpadeo en modo CONFIG     */
#define LED_LEVEL  40      /* Brillo (0–255) — bajo para no encandilar */

static const char *TAG = "LED";
static led_strip_handle_t  s_strip;
static volatile led_state_t s_state = LED_ST_OFF;

/* Fija el color del único píxel y refresca */
static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

/* Tarea que dibuja el estado actual del LED */
static void led_task(void *pvParameters)
{
    bool on = false;
    while (1) {
        switch (s_state) {
            case LED_ST_CONFIG:                 /* Rojo parpadeando */
                on = !on;
                if (on) set_rgb(LED_LEVEL, 0, 0);
                else    led_strip_clear(s_strip);
                vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
                break;

            case LED_ST_CONNECTED:              /* Azul fijo */
                set_rgb(0, 0, LED_LEVEL);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case LED_ST_OFF:
            default:
                led_strip_clear(s_strip);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
}

void status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num   = LED_GPIO,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,   /* WS2812 = orden GRB */
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,      /* 10 MHz: 0.1 µs por tick */
        .mem_block_symbols = 0,
        .flags.with_dma    = false,
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar el LED WS2812 (GPIO%d)", LED_GPIO);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "status_led", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "LED de estado WS2812 inicializado (GPIO%d)", LED_GPIO);
}

void status_led_set(led_state_t st)
{
    s_state = st;
}
