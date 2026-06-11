/* ============================================================
 *   board_pins — mapa físico ESP32-C6 (estación ambiental)
 * ============================================================
 *   Único lugar donde se define a qué GPIO o canal ADC va cada
 *   periférico del proyecto. Cualquier cambio de pinout se hace
 *   acá y todo el firmware se recompila contra el nuevo mapa.
 *   Refleja directamente el esquemático del PCB.
 * ============================================================ */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* ── Sensores ──────────────────────────────────────────── */
#define PIN_DHT22         GPIO_NUM_4    /* One-wire DHT22 (pull-up 4.7 kΩ)     */
#define ADC_CH_GAS        ADC_CHANNEL_1 /* GPIO1 — MQ-2 / potenciómetro      */
#define ADC_CH_TEMP       ADC_CHANNEL_4 /* GPIO4 — NTC simulada (campo temp2)  */

/* ── Actuadores ────────────────────────────────────────── */
#define PIN_FAN_PWM       GPIO_NUM_13   /* Gate MOSFET IRLZ44N (LEDC ch0)      */
#define PIN_LED_ALARM     GPIO_NUM_11   /* LED rojo de alarma                  */
#define PIN_LED_OK        GPIO_NUM_12   /* LED verde de estado normal (OK)     */

/* ── Botón (activo en ALTO, cableado a 3V3, pull-down interno) ── */
#define PIN_BTN_RESET     GPIO_NUM_18   /* Reset config en runtime (long-press)*/

/* ── LED de estado ─────────────────────────────────────── */
#define PIN_STATUS_LED    8             /* WS2812 onboard ESP32-C6-DevKitC-1   */

/* ── Pantalla OLED SSD1306 (I2C) ───────────────────────── */
#define PIN_I2C_SDA       GPIO_NUM_21   /* OLED SDA                            */
#define PIN_I2C_SCL       GPIO_NUM_22   /* OLED SCL                            */
#define OLED_I2C_ADDR     0x3C          /* Dirección por defecto del SSD1306   */
#define OLED_WIDTH        128
#define OLED_HEIGHT       64
