/* ============================================================
 *   oled — wrapper de la pantalla SSD1306 (I2C)
 * ============================================================
 *   Adapta el diseño de la interfaz de usuario del prototipo
 *   Arduino al driver `nopnop2002/esp-idf-ssd1306` (orientado a
 *   filas de 8 px). Layout 128x64 (8 filas, 16 chars):
 *
 *       MONITOR AMBIENTAL   ← encabezado
 *       ----------------
 *       Temp:     XX.X C
 *       Gas:      XXXX
 *       ----------------
 *       EXTRAC: ACTIVADO    ← invertido si alarma
 * ============================================================ */
#pragma once

#include <stdbool.h>

/* Inicializa el bus I2C y la pantalla. Idempotente: una sola vez. */
void oled_init(void);

/* Pantalla de bienvenida al arranque (réplica del prototipo Arduino). */
void oled_show_splash(void);

/* Pantalla de calibración del MQ-2 (baseline en curso). */
void oled_show_calibrating(void);

/* Muestra baseline calibrado + umbrales calculados. */
void oled_show_baseline(int baseline, int gas_on, int gas_off);

/* Pantalla de error de lectura del DHT22. */
void oled_show_dht_error(void);

/* Renderiza temperatura, gas y estado del extractor.
 * temp_c:  temperatura ya seleccionada por el caller (DHT22 o NTC).
 * gas_raw: valor crudo 0..4095 del MQ-2.
 * fan_on:  estado físico del relé del extractor (true = ACTIVADO). */
void oled_render(float temp_c, int gas_raw, bool fan_on);
