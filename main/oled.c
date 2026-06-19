/* ============================================================
 *   oled — implementación SSD1306 sobre I2C
 * ============================================================
 *   El layout sigue el diseño del prototipo Arduino:
 *     - Encabezado " MONITOR AMB."
 *     - Separadores con guiones
 *     - Bloque central con Temperatura y Gas
 *     - Línea inferior con estado del extractor (invertida en alarma)
 * ============================================================ */
#include "oled.h"
#include "board_pins.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG_OLED = "OLED";
static SSD1306_t   s_oled;
static bool        s_ready = false;

/* Escribe una línea fija de 16 caracteres en la página indicada.
 * Pad con espacios para garantizar que cualquier texto previo quede borrado. */
static void show_line(int page, const char *txt, bool invert)
{
    char buf[17];
    int n = snprintf(buf, sizeof(buf), "%-16.16s", txt);
    if (n < 0) return;
    if (n > 16) n = 16;
    ssd1306_display_text(&s_oled, page, buf, n, invert);
}

void oled_init(void)
{
    if (s_ready) return;

    i2c_master_init(&s_oled, PIN_I2C_SDA, PIN_I2C_SCL, -1);
    ssd1306_init(&s_oled, OLED_WIDTH, OLED_HEIGHT);
    ssd1306_clear_screen(&s_oled, false);

    s_ready = true;
    ESP_LOGI(TAG_OLED, "OLED SSD1306 lista (SDA=%d SCL=%d)",
             PIN_I2C_SDA, PIN_I2C_SCL);
}

void oled_show_splash(void)
{
    if (!s_ready) return;

    ssd1306_clear_screen(&s_oled, false);
    show_line(1, " ESTACION  AMB. ", false);
    show_line(3, "    Sistema     ", false);
    show_line(4, "    Iniciado    ", false);
    show_line(6, "  Conectando... ", false);
}

void oled_show_calibrating(void)
{
    if (!s_ready) return;

    ssd1306_clear_screen(&s_oled, false);
    show_line(1, "  CALIBRANDO    ", true);
    show_line(3, "    MQ-2...     ", false);
    show_line(5, "  Espere 30 s   ", false);
    show_line(7, " NO GAS CERCA   ", true);
}

void oled_show_baseline(int baseline, int gas_on, int gas_off)
{
    if (!s_ready) return;

    ssd1306_clear_screen(&s_oled, false);
    show_line(1, "   CALIBRADO!   ", true);
    char line[17];
    snprintf(line, sizeof(line), "Baseline: %4d  ", baseline);
    show_line(3, line, false);
    snprintf(line, sizeof(line), "ON: %4d        ", gas_on);
    show_line(4, line, false);
    snprintf(line, sizeof(line), "OFF: %4d       ", gas_off);
    show_line(5, line, false);
}

void oled_show_dht_error(void)
{
    if (!s_ready) return;

    ssd1306_clear_screen(&s_oled, false);
    show_line(2, "  ERROR SENSOR  ", true);
    show_line(4, "     DHT22      ", true);
}

void oled_render(float temp_c, int gas_raw, bool fan_on)
{
    if (!s_ready) return;

    char line[17];

    /* Sin clear_screen para evitar parpadeo: cada fila se reescribe en sitio
     * y las filas "vacías" se sobrescriben con espacios para borrar contenido
     * previo (splash, error de DHT, etc.).                                  */

    /* Encabezado */
    show_line(0, "  MONITOR AMB.  ", false);
    show_line(1, "----------------", false);

    /* Fila vacía entre separador y temperatura */
    show_line(2, "                ", false);

    /* Lecturas */
    snprintf(line, sizeof(line), "Temp:    %5.1fC", temp_c);
    show_line(3, line, false);

    /* Fila vacía entre temperatura y gas */
    show_line(4, "                ", false);

    snprintf(line, sizeof(line), "Gas:     %5d  ", gas_raw);
    show_line(5, line, false);

    /* Separador inferior y estado físico del extractor (invertido si encendido) */
    show_line(6, "----------------", false);
    show_line(7,
              fan_on ? "EXTRAC: ACTIVADO" : "EXTRAC: APAGADO ",
              fan_on);
}
