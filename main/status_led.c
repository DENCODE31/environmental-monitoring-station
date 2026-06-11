/* ============================================================
 *   status_led — implementación
 * ============================================================ */
#include "status_led.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"

#define LED_LEVEL  40      /* Brillo (0–255) — bajo para no encandilar */

/* Periodos de parpadeo por estado (medio ciclo, ms) */
#define BLINK_PORTAL_MS        500   /* Rojo lento  → 1 Hz  */
#define BLINK_RECONNECT_MS     150   /* Naranja     → 3 Hz  */
#define BLINK_RESET_HOLD_MS     80   /* Magenta     → ~6 Hz */

static const char         *TAG = "LED";
static led_strip_handle_t  s_strip;

/* Estado actual y estado previo (para push/pop durante el RESET_HOLD). */
static volatile led_state_t s_state       = LED_ST_OFF;
static volatile led_state_t s_saved_state = LED_ST_OFF;

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

            case LED_ST_PORTAL:                 /* Rojo lento (1 Hz) */
                on = !on;
                if (on) set_rgb(LED_LEVEL, 0, 0);
                else    led_strip_clear(s_strip);
                vTaskDelay(pdMS_TO_TICKS(BLINK_PORTAL_MS));
                break;

            case LED_ST_RECONNECTING:           /* Naranja rápido (3 Hz) */
                on = !on;
                if (on) set_rgb(LED_LEVEL, LED_LEVEL / 3, 0);
                else    led_strip_clear(s_strip);
                vTaskDelay(pdMS_TO_TICKS(BLINK_RECONNECT_MS));
                break;

            case LED_ST_RESET_HOLD:             /* Magenta pulsando (6 Hz) */
                on = !on;
                if (on) set_rgb(LED_LEVEL, 0, LED_LEVEL);
                else    led_strip_clear(s_strip);
                vTaskDelay(pdMS_TO_TICKS(BLINK_RESET_HOLD_MS));
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
        .strip_gpio_num   = PIN_STATUS_LED,
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
        ESP_LOGE(TAG, "No se pudo inicializar el LED WS2812 (GPIO%d)", PIN_STATUS_LED);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "status_led", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "LED de estado WS2812 inicializado (GPIO%d)", PIN_STATUS_LED);
}

void status_led_set(led_state_t st)
{
    s_state = st;
}

void status_led_push_state(led_state_t st)
{
    s_saved_state = s_state;
    s_state       = st;
}

void status_led_pop_state(void)
{
    s_state = s_saved_state;
}
