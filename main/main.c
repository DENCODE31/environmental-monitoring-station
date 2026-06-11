/* ============================================================
 *   Estación de Monitoreo Ambiental — Control de Extractor
 * ============================================================
 *   Lee temperatura (DHT22) y nivel de gases combustibles (MQ-2).
 *   Acciona extractor 12V vía MOSFET (ON/OFF con histéresis) cuando gas > umbral.
 *   Credenciales WiFi y umbrales configurables por portal cautivo (sin reflashear).
 *   Publica datos por MQTT sobre TLS a AWS IoT Core + broker emqx (dashboard web).
 *   Usa Device Shadow clásica para recordar el estado tras corte de luz.
 *   Indica estado de red con LED RGB, alarma con LED rojo y estado OK con LED verde.
 *   Muestra Temperatura / Gas / estado del extractor en pantalla OLED SSD1306 (I2C).
 *   Arquitectura lista para PID por PWM (LEDC) en fase 2.
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "mqtt_client.h"
#include "esp_netif_sntp.h"               /* Sincronización de hora (TLS requiere reloj válido) */
#include "esp_netif.h"                    /* Para forzar DNS fallback (AWS IoT)  */
#include "lwip/ip_addr.h"                 /* Helpers IPv4 para esp_netif_dns_info_t */
#include "cJSON.h"                        /* Parseo del JSON de la Device Shadow */
#include "rom/ets_sys.h"
#include "app_config.h"                   /* Configuración persistente en NVS (WiFi/AWS/umbrales) */
#include "provisioning.h"                 /* Portal cautivo de configuración WiFi */
#include "status_led.h"                   /* LED RGB de estado (WS2812 onboard) */
#include "board_pins.h"                   /* Mapa físico GPIO/ADC del ESP32-C6  */
#include "oled.h"                         /* Pantalla SSD1306 (I2C)             */

/* ── Certificados AWS IoT embebidos en flash (ver main/certs/, no versionados) ──
 * EMBED_TXTFILES los inserta null-terminados → usables como cadenas C.        */
extern const uint8_t aws_root_ca_pem_start[]     asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_device_cert_pem_start[] asm("_binary_aws_device_cert_pem_start");
extern const uint8_t aws_device_key_pem_start[]  asm("_binary_aws_device_key_pem_start");

/* ── Credenciales WiFi ─────────────────────────────────────── */
/* La lista de redes vive en NVS (g_cfg). Se configura por el portal cautivo. */
#define WIFI_MAX_RETRY 10                 /* Intentos antes de reiniciar */

/* ── AWS IoT Core — MQTT sobre TLS (puerto 8883) ───────────── */
/* Endpoint y thing name se cargan de NVS (g_cfg) y se arman en runtime. */
#define MQTT_TOPIC_DATA    "ems/data"                  /* Publicación de sensores   */
#define MQTT_TOPIC_STATUS  "ems/status"                /* LWT online/offline        */
#define MQTT_TOPIC_CMD     "ems/cmd"                   /* Comandos entrantes        */
#define MQTT_TOPIC_CFG     "ems/cfg"                   /* Estado actual de umbrales */
#define MQTT_TOPIC_CFG_SET "ems/cfg/set"               /* Solicitud de cambio de umbrales */

/* ── Device Shadow clásica — recall de estado tras corte de luz ──
 * El ESP32 no guarda estado en flash: al arrancar pregunta a la nube
 * "¿en qué estado estaba?" y se restaura desde el último 'reported'.
 * URI, client_id y topics de sombra se construyen en aws_build_endpoints()
 * a partir del endpoint/thing configurados en NVS.                        */
static char s_mqtt_uri[160];                           /* mqtts://<endpoint>:8883          */
static char s_client_id[CFG_THING_LEN];                /* = thing name                     */
static char s_shadow_get[96];                          /* $aws/things/<thing>/shadow/get    */
static char s_shadow_get_accepted[112];                /* .../shadow/get/accepted           */
static char s_shadow_update[96];                       /* .../shadow/update                 */
static char s_shadow_delta[112];                       /* .../shadow/update/delta           */

/* Construye URI MQTT, client_id y topics de sombra desde la config NVS */
static void aws_build_endpoints(void)
{
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtts://%s:8883", g_cfg.aws_endpoint);
    strlcpy(s_client_id, g_cfg.aws_thing, sizeof(s_client_id));
    snprintf(s_shadow_get,          sizeof(s_shadow_get),          "$aws/things/%s/shadow/get",              g_cfg.aws_thing);
    snprintf(s_shadow_get_accepted, sizeof(s_shadow_get_accepted), "$aws/things/%s/shadow/get/accepted",     g_cfg.aws_thing);
    snprintf(s_shadow_update,       sizeof(s_shadow_update),       "$aws/things/%s/shadow/update",           g_cfg.aws_thing);
    snprintf(s_shadow_delta,        sizeof(s_shadow_delta),        "$aws/things/%s/shadow/update/delta",     g_cfg.aws_thing);
}

/* ── Broker MQTT secundario (emqx) para el dashboard local ──────
 * AWS IoT = capa segura/almacenamiento; emqx (público, TCP) alimenta
 * el dashboard web que se suscribe por websocket al mismo broker.   */
#define EMQX_BROKER_URI  "mqtt://broker.emqx.io:1883"
#define EMQX_CLIENT_ID   "esp32c6_ems_pub"

/* ── Fuente de temperatura ──────────────────────────────────────
 * 1: NTC analógica en GPIO4 (ADC_CH4) → campo temp2 (simulación).
 * 0: DHT22 digital en GPIO4 → temp + humedad reales (maqueta final).
 * Requisito de hardware con DHT22: pull-up 4.7-10 kΩ entre DATA y 3V3. */
#define USE_NTC_SIM  0

/* ── Cadencia ──────────────────────────────────────────────────
 * Control/sensado cada 1 s (respuesta rápida de seguridad).
 * Publicación de telemetría cada 10 s para no exceder el free tier. */
#define CONTROL_PERIOD_MS 1000
#define PUBLISH_EVERY_N   10

/* ── Pines y canales del MCU ───────────────────────────────
 * Mapa físico completo en board_pins.h. Acá solo van los
 * parámetros lógicos asociados al botón de reset.            */
#define BTN_LONGPRESS_MS 3000          /* Tiempo de pulsación larga (ms)   */

/* Escala estilo LM35 que espera el dashboard para temp2 (0..~330) */
#define NTC_ADC_TO_TEMP2(raw)  ((float)(raw) * 3300.0f / 4095.0f / 10.0f)

/* ── LEDC (PWM extractor) ──────────────────────────────────── */
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL     LEDC_CHANNEL_0
#define LEDC_DUTY_RES    LEDC_TIMER_10_BIT  /* Resolución: 0–1023     */
#define LEDC_FREQ_HZ     25000              /* 25 kHz, inaudible      */

/* ── Umbrales de alarma ────────────────────────────────────── */
/* Configurables en campo desde el portal cautivo. Se leen de g_cfg:
 * g_cfg.gas_on (enciende), g_cfg.gas_off (histéresis), g_cfg.temp_alarm (°C). */

/* ── DHT22: timing (µs) ────────────────────────────────────── */
#define DHT22_START_LOW_US   1100  /* Pulso inicio host → sensor  */
#define DHT22_RESP_WAIT_US   40    /* Espera respuesta del sensor  */
#define DHT22_BIT_TIMEOUT_US 100   /* Timeout por bit              */

/* ── Tags de log ───────────────────────────────────────────── */
static const char *TAG_MAIN  = "EMS";
static const char *TAG_WIFI  = "WIFI";
static const char *TAG_MQTT  = "MQTT";
static const char *TAG_ADC   = "ADC";
#if !USE_NTC_SIM
static const char *TAG_DHT   = "DHT22";   /* Solo en modo maqueta (DHT22 real) */
#endif

/* ── Estado global compartido entre tareas ─────────────────── */
typedef struct {
    float    humidity;       /* Humedad relativa en % — DHT22         */
    float    temp_dht;       /* Temperatura °C — DHT22 (digital)      */
    float    temp2;          /* Temperatura — NTC ADC1_CH4 (campo temp2 del dashboard) */
    int      gas_raw;        /* ADC raw — MQ-2 ADC1_CH1 (GPIO1)     */
    bool     fan_on;         /* Estado del extractor                  */
    uint32_t fan_pwm;        /* Ciclo de trabajo 0–1023               */
    bool     alarm_active;   /* Bandera de alarma                     */
} sensor_state_t;

static sensor_state_t g_state = {0};  /* Estado global inicializado a cero */

static volatile bool g_emergency_stop = false;  /* Paro de emergencia: fuerza extractor OFF */

/* ── Handles de FreeRTOS ───────────────────────────────────── */
static EventGroupHandle_t  s_wifi_event_group;  /* Grupo de eventos WiFi */
static esp_mqtt_client_handle_t s_mqtt_client;  /* Handle del cliente MQTT (AWS IoT) */
static volatile bool s_mqtt_connected = false;  /* true cuando hay sesión con AWS IoT */
static esp_mqtt_client_handle_t s_emqx_client;  /* Handle del cliente MQTT (emqx, dashboard) */
static volatile bool s_emqx_connected = false;  /* true cuando hay sesión con emqx */

/* ── ADC ───────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle;  /* Handle del ADC oneshot */

/* Bits de evento WiFi */
#define WIFI_CONNECTED_BIT BIT0  /* Set cuando conectado */
#define WIFI_FAIL_BIT      BIT1  /* Set cuando falla definitivamente */

static int s_retry_num = 0;  /* Contador de reintentos WiFi */


/* ════════════════════════════════════════════════════════════
 *   DRIVER DHT22 — bit-bang sobre GPIO4 (solo en modo maqueta)
 * ════════════════════════════════════════════════════════════ */
#if !USE_NTC_SIM

/* Espera activa hasta que el pin alcance 'level' o vence el timeout.
 * Retorna µs transcurridos, o -1 si hubo timeout. */
static int dht22_wait_level(int level, int timeout_us)
{
    int elapsed = 0;
    /* Sondeo cada ~1 µs hasta detectar el nivel esperado */
    while (gpio_get_level(PIN_DHT22) != level) {
        if (++elapsed >= timeout_us) {
            return -1;  /* Timeout — sensor no respondió */
        }
        ets_delay_us(1);  /* Espera 1 µs entre muestras */
    }
    return elapsed;
}

/* Lee temperatura y humedad del DHT22.
 * Retorna ESP_OK si la transmisión y CRC son correctos. */
static esp_err_t dht22_read(float *out_temp, float *out_hum)
{
    uint8_t data[5] = {0};  /* 40 bits: hum_hi, hum_lo, tmp_hi, tmp_lo, cksum */

    /* ── 1. Señal de inicio: host baja el pin ≥ 1 ms ───────── */
    gpio_set_direction(PIN_DHT22, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DHT22, 0);
    ets_delay_us(DHT22_START_LOW_US);           /* Mantiene bajo 1.1 ms */
    gpio_set_level(PIN_DHT22, 1);
    ets_delay_us(30);                           /* Sube y espera 30 µs  */
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);

    /* ── 2. Respuesta del sensor: 80 µs bajo + 80 µs alto ──── */
    if (dht22_wait_level(0, 100) < 0) {         /* Espera flanco bajo   */
        ESP_LOGE(TAG_DHT, "Sin respuesta (flanco bajo)");
        return ESP_ERR_TIMEOUT;
    }
    if (dht22_wait_level(1, 100) < 0) {         /* Espera flanco alto   */
        ESP_LOGE(TAG_DHT, "Sin respuesta (flanco alto)");
        return ESP_ERR_TIMEOUT;
    }
    if (dht22_wait_level(0, 100) < 0) {         /* Espera fin preámbulo */
        ESP_LOGE(TAG_DHT, "Sin respuesta (fin preambulo)");
        return ESP_ERR_TIMEOUT;
    }

    /* ── 3. Leer 40 bits de datos ───────────────────────────── */
    for (int i = 0; i < 40; i++) {
        /* Cada bit inicia con 50 µs bajo */
        if (dht22_wait_level(1, DHT22_BIT_TIMEOUT_US) < 0) {
            ESP_LOGE(TAG_DHT, "Timeout esperando bit %d alto", i);
            return ESP_ERR_TIMEOUT;
        }
        /* Mide duración del pulso alto: <28 µs=0, ~70 µs=1 */
        int width = dht22_wait_level(0, DHT22_BIT_TIMEOUT_US);
        if (width < 0) {
            ESP_LOGE(TAG_DHT, "Timeout midiendo bit %d", i);
            return ESP_ERR_TIMEOUT;
        }
        data[i / 8] <<= 1;         /* Desplaza el byte actual    */
        if (width > 35) {           /* Umbral a 35 µs             */
            data[i / 8] |= 1;       /* Bit alto si pulso > 35 µs  */
        }
    }

    /* ── 4. Verificación de checksum ───────────────────────── */
    uint8_t cksum = data[0] + data[1] + data[2] + data[3];
    if (cksum != data[4]) {
        ESP_LOGE(TAG_DHT, "Checksum error: calc=0x%02X recv=0x%02X", cksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    /* ── 5. Decodificar valores ─────────────────────────────── */
    /* Humedad: word de 16 bits, factor 0.1 */
    *out_hum  = ((data[0] << 8) | data[1]) * 0.1f;

    /* Temperatura: 15 bits + bit de signo en MSB del byte alto */
    int16_t raw_temp = ((data[2] & 0x7F) << 8) | data[3];
    *out_temp = raw_temp * 0.1f;
    if (data[2] & 0x80) {           /* Bit de signo → temperatura negativa */
        *out_temp = -*out_temp;
    }

    ESP_LOGD(TAG_DHT, "Temp=%.1f°C  Hum=%.1f%%", *out_temp, *out_hum);
    return ESP_OK;
}

#endif /* !USE_NTC_SIM */


/* ════════════════════════════════════════════════════════════
 *   ADC — MQ-2 en ADC1_CH1 (GPIO1)
 * ════════════════════════════════════════════════════════════ */

/* Inicializa ADC1: CH1 = MQ-2 (GPIO1) */
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {        /* Configuración base del ADC */
        .unit_id = ADC_UNIT_1,                         /* Usar ADC1 (compatible con WiFi activo) */
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));  /* Crear handle */

    adc_oneshot_chan_cfg_t chan_config = {              /* Configuración compartida por ambos canales */
        .bitwidth = ADC_BITWIDTH_12,                   /* Resolución 12 bits (0–4095) */
        .atten    = ADC_ATTEN_DB_12,                   /* Atenuación 12 dB -> rango 0-3.3 V */
    };

    /* Canal 1 (GPIO1) — MQ-2 / potenciómetro: gas */
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        s_adc_handle, ADC_CH_GAS, &chan_config));

#if USE_NTC_SIM
    /* Canal 4 (GPIO4) — NTC: temperatura analógica (campo temp2) */
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        s_adc_handle, ADC_CH_TEMP, &chan_config));
    ESP_LOGI(TAG_ADC, "ADC1 CH1 (gas) y CH4 (NTC temp2) inicializados");
#else
    ESP_LOGI(TAG_ADC, "ADC1 CH1 (gas) inicializado");
#endif
}

/* Lee el MQ-2 en ADC1_CH1 (GPIO1). Retorna raw 0–4095. */
static int adc_read_gas(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CH_GAS, &raw));   /* Lectura MQ-2 */
    ESP_LOGD(TAG_ADC, "MQ-2 CH1 raw=%d", raw);
    return raw;
}

#if USE_NTC_SIM
/* Lee la NTC en ADC1_CH4 (GPIO4) y la devuelve en escala temp2 (estilo LM35). */
static float adc_read_temp2(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CH_TEMP, &raw));  /* Lectura NTC */
    float t = NTC_ADC_TO_TEMP2(raw);
    ESP_LOGD(TAG_ADC, "NTC CH4 raw=%d → temp2=%.1f", raw, t);
    return t;
}
#endif

/* ════════════════════════════════════════════════════════════
 *   LEDC — PWM para extractor (GPIO10)
 * ════════════════════════════════════════════════════════════ */

/* Configura el timer y canal LEDC para el extractor */
static void ledc_fan_init(void)
{
    /* Configurar timer LEDC */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_MODE,       /* Modo low-speed      */
        .timer_num       = LEDC_TIMER,      /* Timer 0             */
        .duty_resolution = LEDC_DUTY_RES,   /* Resolución 10 bits  */
        .freq_hz         = LEDC_FREQ_HZ,    /* 25 kHz inaudible    */
        .clk_cfg         = LEDC_AUTO_CLK,   /* Selección automática de fuente */
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));  /* Aplicar config del timer */

    /* Configurar canal LEDC en GPIO10 */
    ledc_channel_config_t channel_cfg = {
        .speed_mode = LEDC_MODE,       /* Mismo modo que el timer  */
        .channel    = LEDC_CHANNEL,    /* Canal 0                  */
        .timer_sel  = LEDC_TIMER,      /* Asociar al timer 0       */
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_FAN_PWM,    /* GPIO10 → Gate MOSFET     */
        .duty       = 0,               /* Arranca en 0% (apagado)  */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));  /* Aplicar config del canal */

    ESP_LOGI(TAG_MAIN, "LEDC fan PWM inicializado (GPIO%d, %d Hz)", PIN_FAN_PWM, LEDC_FREQ_HZ);
}

/* Establece el ciclo de trabajo del extractor (0–1023) */
static void fan_set_duty(uint32_t duty)
{
    duty = duty > 1023 ? 1023 : duty;              /* Clamp al máximo     */
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);  /* Aplicar duty cycle  */
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);     /* Refrescar hardware  */
}


/* ════════════════════════════════════════════════════════════
 *   GPIO — LED rojo de alarma + LED verde de estado normal
 * ════════════════════════════════════════════════════════════ */

/* Configura ambos LEDs como salidas digitales. Estado inicial:
 * verde encendido (todo OK), rojo apagado. */
static void alarm_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LED_ALARM) | (1ULL << PIN_LED_OK),
        .mode         = GPIO_MODE_OUTPUT,      /* Salida digital          */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(PIN_LED_ALARM, 0);  /* Rojo apagado al inicio */
    gpio_set_level(PIN_LED_OK,    1);  /* Verde encendido (estado OK por defecto) */

    ESP_LOGI(TAG_MAIN, "LED alarma rojo (GPIO%d) y LED OK verde (GPIO%d) inicializados",
             PIN_LED_ALARM, PIN_LED_OK);
}

/* Indica estado del sistema: alarma activa enciende rojo y apaga verde;
 * estado normal enciende verde y apaga rojo. */
static void alarm_set(bool active)
{
    gpio_set_level(PIN_LED_ALARM, active ? 1 : 0);  /* Rojo: ON en alarma  */
    gpio_set_level(PIN_LED_OK,    active ? 0 : 1);  /* Verde: ON en normal */
}


/* ════════════════════════════════════════════════════════════
 *   BOTÓN — reset de configuración por long-press en runtime
 * ════════════════════════════════════════════════════════════ */

/* Configura el botón como entrada con pull-down (activo en ALTO).
 * Hardware: botón conecta 3V3 → GPIO18 cuando se presiona.
 * Sin pulsar el pull-down interno mantiene la línea en bajo. */
static void buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_RESET),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* Sin botón → lee bajo (no presionado) */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    /* Refuerzo del pull-down por si gpio_config no lo fija inmediatamente. */
    gpio_set_pull_mode(PIN_BTN_RESET, GPIO_PULLDOWN_ONLY);
}

/* Tarea: vigila el botón de reset en operación normal. Mientras se
 * sostiene el botón, el LED RGB pulsa magenta para indicar la cuenta.
 * Si se libera antes de BTN_LONGPRESS_MS se vuelve al estado anterior;
 * si se completa el long-press se borran credenciales y se reinicia.   */
static void factory_button_task(void *pvParameters)
{
    int  held_ms     = 0;
    bool hold_active = false;  /* Indica si ya empujamos RESET_HOLD al LED */
    int  prev_level  = -1;     /* Para diagnóstico: imprime cada flanco    */

    /* Pausa de estabilización: el pull-up interno tarda un instante en
     * fijar la línea en alto tras gpio_config. Sin esta espera la primera
     * lectura puede dar 0 espuriamente y disparar un magenta fantasma.    */
    vTaskDelay(pdMS_TO_TICKS(300));

    while (1) {
        int level = gpio_get_level(PIN_BTN_RESET);

        if (level != prev_level) {
            ESP_LOGI(TAG_MAIN, "RESET btn GPIO%d nivel=%d (%s)",
                     PIN_BTN_RESET, level,
                     level == 1 ? "PRESIONADO" : "liberado");
            prev_level = level;
        }

        if (level == 1) {                            /* Presionado (activo ALTO, cableado a 3V3) */
            if (!hold_active) {
                /* Primer tick presionado: cambiar el RGB a magenta y guardar estado actual */
                status_led_push_state(LED_ST_RESET_HOLD);
                hold_active = true;
            }
            held_ms += 100;

            if (held_ms >= BTN_LONGPRESS_MS) {
                ESP_LOGW(TAG_MAIN, "Botón reset %d s — borrando credenciales y reiniciando al portal",
                         BTN_LONGPRESS_MS / 1000);
                /* Feedback final: 3 parpadeos alternando rojo/verde para confirmar */
                for (int i = 0; i < 3; i++) {
                    alarm_set(true);  vTaskDelay(pdMS_TO_TICKS(80));
                    alarm_set(false); vTaskDelay(pdMS_TO_TICKS(80));
                }
                app_config_factory_reset();
                esp_restart();
            }
        } else {
            /* Botón liberado */
            if (hold_active) {
                status_led_pop_state();              /* Restaurar estado de red previo */
                hold_active = false;
            }
            held_ms = 0;                              /* Soltó antes de tiempo → cancelado */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/* ════════════════════════════════════════════════════════════
 *   WIFI — conexión con reconexión automática y multi-red
 * ════════════════════════════════════════════════════════════ */

/* Redes conocidas en g_cfg (NVS). El firmware escanea y elige la primera
 * disponible según el orden de la lista (prioridad).                       */

/* Escanea el aire, elige la primera red conocida presente, fija sus
 * credenciales y lanza la conexión. Retorna ESP_OK si encontró una. */
static esp_err_t wifi_connect_best_ap(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {  /* Escaneo bloqueante */
        return ESP_FAIL;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGW(TAG_WIFI, "Escaneo: ninguna red detectada");
        return ESP_FAIL;
    }

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!recs) return ESP_ERR_NO_MEM;
    esp_wifi_scan_get_ap_records(&ap_num, recs);

    /* Primera red conocida presente, respetando el orden de prioridad */
    int chosen = -1;
    for (int i = 0; i < g_cfg.ap_count && chosen < 0; i++) {
        for (int j = 0; j < ap_num; j++) {
            if (strcmp((char *) recs[j].ssid, g_cfg.aps[i].ssid) == 0) {
                chosen = i;
                break;
            }
        }
    }
    free(recs);

    if (chosen < 0) {
        ESP_LOGW(TAG_WIFI, "Ninguna red conocida visible (%d escaneadas)", ap_num);
        return ESP_FAIL;
    }

    wifi_config_t cfg = { 0 };
    strlcpy((char *) cfg.sta.ssid,     g_cfg.aps[chosen].ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *) cfg.sta.password, g_cfg.aps[chosen].pass, sizeof(cfg.sta.password));
    /* Red abierta si no hay contraseña; si no, exigir al menos WPA2 */
    cfg.sta.threshold.authmode = g_cfg.aps[chosen].pass[0]
                               ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    ESP_LOGI(TAG_WIFI, "Red elegida: '%s' — conectando...", g_cfg.aps[chosen].ssid);
    esp_wifi_connect();
    return ESP_OK;
}

/* Handler de eventos WiFi e IP */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* La conexión la dispara wifi_connect_best_ap() tras escanear */

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        status_led_set(LED_ST_RECONNECTING);  /* Naranja rápido: sin enlace WiFi */
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();             /* Reintentar conexión  */
            s_retry_num++;
            ESP_LOGW(TAG_WIFI, "Reintento WiFi %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);  /* Falló definitivamente */
            ESP_LOGE(TAG_WIFI, "Conexión WiFi fallida tras %d intentos", WIFI_MAX_RETRY);
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG_WIFI, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));

        /* Forzar DNS público de respaldo. En redes (hotspots/captive portals)
         * donde DHCP entrega DNS roto o nulo, esto evita el clásico
         * `getaddrinfo() returns 202` al resolver pool.ntp.org / AWS IoT.    */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_dns_info_t dns_main = {0}, dns_fallback = {0};
            dns_main.ip.type = ESP_IPADDR_TYPE_V4;
            dns_main.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
            dns_fallback.ip.type = ESP_IPADDR_TYPE_V4;
            dns_fallback.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN,     &dns_main);
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_FALLBACK, &dns_fallback);
            ESP_LOGI(TAG_WIFI, "DNS forzado a 8.8.8.8 (main) / 1.1.1.1 (fallback)");
        }

        s_retry_num = 0;                                              /* Reset contador */
        status_led_set(LED_ST_CONNECTED);                            /* Azul fijo: conectado */
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);  /* Señalizar éxito */
    }
}

/* Inicializa y conecta el WiFi en modo STA.
 * Bloquea hasta conectar o agotar reintentos. */
static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();  /* Grupo de eventos para sincronización */

    ESP_ERROR_CHECK(esp_netif_init());                     /* Inicializar stack TCP/IP  */
    ESP_ERROR_CHECK(esp_event_loop_create_default());      /* Loop de eventos del sistema */
    esp_netif_create_default_wifi_sta();                   /* Interfaz de red WiFi STA  */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();   /* Config por defecto del driver */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Registrar handlers para eventos WiFi e IP */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));       /* Modo estación */
    ESP_ERROR_CHECK(esp_wifi_start());                        /* Arrancar WiFi  */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));           /* Sin modem sleep → MQTT sin latencia */

    /* Hasta 4 rondas: escanear, elegir red conocida presente, conectar */
    for (int round = 0; round < 4; round++) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_num = 0;

        if (wifi_connect_best_ap() != ESP_OK) {
            ESP_LOGW(TAG_WIFI, "Sin red conocida, reintentando escaneo (%d/4)", round + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* Esperar a conectar o agotar reintentos de esta red */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));
        if (bits & WIFI_CONNECTED_BIT) {
            return ESP_OK;
        }
        ESP_LOGW(TAG_WIFI, "Conexión fallida, reintentando (%d/4)", round + 1);
    }

    ESP_LOGE(TAG_WIFI, "No se pudo conectar a ninguna red conocida");
    return ESP_FAIL;
}


/* ════════════════════════════════════════════════════════════
 *   SNTP — sincronización de hora (obligatoria para TLS)
 * ════════════════════════════════════════════════════════════ */

/* Sincroniza el reloj por NTP. El handshake TLS valida la fecha del
 * certificado del servidor; sin hora correcta, la conexión a AWS falla. */
static void time_sync_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);                              /* Arranca cliente SNTP */

    /* Reintenta hasta ~50 s: hotspot celular puede tardar en sincronizar */
    const int max_retry = 10;
    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) != ESP_OK && ++retry < max_retry) {
        ESP_LOGW(TAG_MAIN, "Esperando sincronizacion NTP... (%d/%d)", retry, max_retry);
    }

    /* Validar que el reloj realmente quedó en una fecha plausible (>= 2024) */
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year >= (2024 - 1900)) {
        ESP_LOGI(TAG_MAIN, "Hora sincronizada por NTP (epoch=%lld)", (long long) now);
    } else {
        ESP_LOGW(TAG_MAIN, "NTP no sincronizo tras %d intentos — TLS puede fallar", max_retry);
    }
}


/* ════════════════════════════════════════════════════════════
 *   MQTT — cliente con LWT y reconexión
 * ════════════════════════════════════════════════════════════ */

/* Compara el topic de un evento MQTT (no null-terminado) con una cadena fija */
static bool mqtt_topic_is(esp_mqtt_event_handle_t event, const char *topic)
{
    return event->topic_len == (int) strlen(topic) &&
           strncmp(event->topic, topic, event->topic_len) == 0;
}

/* ── Device Shadow ─────────────────────────────────────────────
 * Publica el estado actual como 'reported' en la sombra. AWS lo
 * guarda de forma persistente; sobrevive a cortes de energía.    */
static void shadow_report(void)
{
    if (!s_mqtt_connected) return;  /* Sin sesión con AWS no se puede publicar */
    char payload[160];
    int len = snprintf(payload, sizeof(payload),
        "{\"state\":{\"reported\":{\"emergency_stop\":%s,\"fan_on\":%s,\"gas\":%d}}}",
        g_emergency_stop ? "true" : "false",
        g_state.fan_on   ? "true" : "false",
        g_state.gas_raw);
    esp_mqtt_client_publish(s_mqtt_client, s_shadow_update, payload, len, 1, 0);  /* QoS 1 */
    ESP_LOGI(TAG_MQTT, "Sombra → reported: %s", payload);
}

/* Pide a AWS el último estado guardado (publica vacío a shadow/get) */
static void shadow_request_get(void)
{
    esp_mqtt_client_publish(s_mqtt_client, s_shadow_get, "", 0, 1, 0);
    ESP_LOGI(TAG_MQTT, "Sombra → solicitando estado guardado (get)");
}

/* Restaura el estado desde el 'reported' recibido en shadow/get/accepted.
 * Aquí ocurre el recall tras corte de luz: leemos lo último que reportamos. */
static void shadow_apply_reported(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG_MQTT, "Sombra get/accepted: JSON inválido");
        return;
    }
    cJSON *state    = cJSON_GetObjectItem(root, "state");
    cJSON *reported = state ? cJSON_GetObjectItem(state, "reported") : NULL;
    if (reported) {
        cJSON *es = cJSON_GetObjectItem(reported, "emergency_stop");
        if (cJSON_IsBool(es)) {
            g_emergency_stop = cJSON_IsTrue(es);
            ESP_LOGW(TAG_MQTT, "Estado RESTAURADO desde sombra: emergency_stop=%d",
                     g_emergency_stop);
        }
    } else {
        ESP_LOGI(TAG_MQTT, "Sombra vacía (primer arranque) — sin estado previo");
    }
    cJSON_Delete(root);
}

/* Aplica un cambio 'desired' recibido en shadow/update/delta.
 * Permite controlar el dispositivo remotamente desde la nube/app.        */
static void shadow_apply_delta(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    cJSON *state = cJSON_GetObjectItem(root, "state");  /* El delta trae el diff en 'state' */
    if (state) {
        cJSON *es = cJSON_GetObjectItem(state, "emergency_stop");
        if (cJSON_IsBool(es)) {
            g_emergency_stop = cJSON_IsTrue(es);
            ESP_LOGW(TAG_MQTT, "Delta de sombra → emergency_stop=%d", g_emergency_stop);
        }
    }
    cJSON_Delete(root);
    shadow_report();  /* Confirmar el cambio reportando el nuevo estado */
}

/* Publica el snapshot actual de umbrales en ems/cfg (retain=1) para
 * que cualquier dashboard que se suscriba lea los valores vigentes.   */
static void publish_cfg_snapshot(void)
{
    char payload[120];
    int len = snprintf(payload, sizeof(payload),
        "{\"gas_on\":%d,\"gas_off\":%d,\"temp_alarm\":%.1f}",
        g_cfg.gas_on, g_cfg.gas_off, g_cfg.temp_alarm);
    if (len <= 0 || len >= (int) sizeof(payload)) return;

    /* emqx con retain → el dashboard al conectarse recibe el estado actual. */
    if (s_emqx_connected) {
        esp_mqtt_client_publish(s_emqx_client, MQTT_TOPIC_CFG, payload, len, 1, 1);
    }
    /* AWS: sin retain (iot:RetainPublish requiere permiso especial). */
    if (s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_CFG, payload, len, 1, 0);
    }
    ESP_LOGI(TAG_MQTT, "Umbrales publicados → %s", payload);
}

/* Aplica un cambio de umbrales recibido por MQTT (ems/cfg/set).
 * Valida rangos, persiste a NVS y republica el snapshot en ems/cfg. */
static void apply_cfg_update(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG_MQTT, "cfg/set: JSON inválido");
        return;
    }
    bool changed = false;

    cJSON *jgo = cJSON_GetObjectItem(root, "gas_on");
    if (cJSON_IsNumber(jgo)) {
        int v = jgo->valueint;
        if (v < 0)    v = 0;
        if (v > 4095) v = 4095;
        g_cfg.gas_on  = v;
        g_cfg.gas_off = v * 85 / 100;   /* Histéresis del 85% */
        changed = true;
    }
    cJSON *jta = cJSON_GetObjectItem(root, "temp_alarm");
    if (cJSON_IsNumber(jta) && jta->valuedouble > 0) {
        g_cfg.temp_alarm = (float) jta->valuedouble;
        changed = true;
    }
    cJSON_Delete(root);

    if (changed) {
        ESP_LOGW(TAG_MQTT, "Umbrales actualizados — gas_on=%d gas_off=%d temp=%.1f",
                 g_cfg.gas_on, g_cfg.gas_off, g_cfg.temp_alarm);
        app_config_save();
        publish_cfg_snapshot();
    }
}

/* Handler de eventos del cliente MQTT */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(TAG_MQTT, "Conectado a AWS IoT Core");
            /* Publicar mensaje de presencia al conectar (sin retain: AWS exige iot:RetainPublish) */
            esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS,
                                    "online", 6, 1, 0);  /* QoS 1, sin retain */
            /* Suscribirse al topic de comandos (paro de emergencia) y al de cambio de umbrales */
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD,     1);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CFG_SET, 1);
            /* Suscribirse a las respuestas de la sombra */
            esp_mqtt_client_subscribe(s_mqtt_client, s_shadow_get_accepted, 1);
            esp_mqtt_client_subscribe(s_mqtt_client, s_shadow_delta, 1);
            /* Pedir el último estado guardado → recall tras corte de luz */
            shadow_request_get();
            /* Publicar snapshot inicial de umbrales */
            publish_cfg_snapshot();
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG_MQTT, "Desconectado de AWS IoT Core");
            break;

        case MQTT_EVENT_DATA:
            if (mqtt_topic_is(event, MQTT_TOPIC_CMD)) {
                /* Comando entrante: "stop" activa paro, "resume" reanuda control auto */
                if (event->data_len == 4 && strncmp(event->data, "stop", 4) == 0) {
                    g_emergency_stop = true;
                    ESP_LOGW(TAG_MQTT, "PARO DE EMERGENCIA activado");
                    shadow_report();   /* Persistir el cambio en la sombra */
                } else if (event->data_len == 6 && strncmp(event->data, "resume", 6) == 0) {
                    g_emergency_stop = false;
                    ESP_LOGI(TAG_MQTT, "Paro de emergencia liberado");
                    shadow_report();   /* Persistir el cambio en la sombra */
                }
            } else if (mqtt_topic_is(event, MQTT_TOPIC_CFG_SET)) {
                /* Cambio de umbrales solicitado por la nube */
                apply_cfg_update(event->data, event->data_len);
            } else if (mqtt_topic_is(event, s_shadow_get_accepted)) {
                /* Respuesta con el último estado guardado → restaurar */
                shadow_apply_reported(event->data, event->data_len);
            } else if (mqtt_topic_is(event, s_shadow_delta)) {
                /* Cambio remoto del estado deseado → aplicar */
                shadow_apply_delta(event->data, event->data_len);
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG_MQTT, "Mensaje publicado (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG_MQTT, "Error MQTT type=%d", event->error_handle->error_type);
            break;

        default:
            break;
    }
}

/* Inicializa y arranca el cliente MQTT con LWT configurado */
static void mqtt_init(void)
{
    /* TLS mutuo: el broker valida el cert del dispositivo y viceversa.
     * LWT: publicar "offline" si el dispositivo se desconecta inesperadamente. */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri     = s_mqtt_uri,        /* mqtts://<endpoint>:8883 */
        .broker.verification.certificate =           /* Root CA → valida al broker */
            (const char *) aws_root_ca_pem_start,
        .credentials.client_id  = s_client_id,       /* ID único = nombre objeto */
        .credentials.authentication.certificate =    /* Cert del dispositivo    */
            (const char *) aws_device_cert_pem_start,
        .credentials.authentication.key =            /* Clave privada           */
            (const char *) aws_device_key_pem_start,
        .session.last_will = {
            .topic   = MQTT_TOPIC_STATUS,            /* Topic del LWT           */
            .msg     = "offline",                    /* Payload del LWT         */
            .msg_len = 7,
            .qos     = 1,                            /* QoS 1 para LWT          */
            .retain  = 0,                            /* Sin retain (AWS exige iot:RetainPublish) */
        },
        .session.keepalive = 60,                     /* Keepalive cada 60 s     */
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);  /* Crear cliente MQTT */
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(   /* Registrar handler de eventos */
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));  /* Iniciar cliente */

    ESP_LOGI(TAG_MQTT, "Cliente MQTT iniciado → %s", s_mqtt_uri);
}

/* Handler del cliente emqx (broker secundario para el dashboard) */
static void emqx_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            s_emqx_connected = true;
            ESP_LOGI(TAG_MQTT, "Conectado a emqx (dashboard)");
            esp_mqtt_client_publish(s_emqx_client, MQTT_TOPIC_STATUS, "online", 6, 1, 0);
            esp_mqtt_client_subscribe(s_emqx_client, MQTT_TOPIC_CMD,     1);
            esp_mqtt_client_subscribe(s_emqx_client, MQTT_TOPIC_CFG_SET, 1);
            publish_cfg_snapshot();   /* Estado actual con retain → el dashboard lo recibe al suscribirse */
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_emqx_connected = false;
            ESP_LOGW(TAG_MQTT, "Desconectado de emqx");
            break;

        case MQTT_EVENT_DATA:
            /* Comandos desde el dashboard (stop/resume) */
            if (mqtt_topic_is(event, MQTT_TOPIC_CMD)) {
                if (event->data_len == 4 && strncmp(event->data, "stop", 4) == 0) {
                    g_emergency_stop = true;
                    ESP_LOGW(TAG_MQTT, "PARO DE EMERGENCIA (dashboard)");
                    shadow_report();   /* Sincronizar a la sombra de AWS */
                } else if (event->data_len == 6 && strncmp(event->data, "resume", 6) == 0) {
                    g_emergency_stop = false;
                    ESP_LOGI(TAG_MQTT, "Paro liberado (dashboard)");
                    shadow_report();
                }
            } else if (mqtt_topic_is(event, MQTT_TOPIC_CFG_SET)) {
                /* Cambio de umbrales solicitado desde el dashboard */
                apply_cfg_update(event->data, event->data_len);
            }
            break;

        default:
            break;
    }
}

/* Inicializa el cliente MQTT secundario hacia emqx (TCP plano, sin TLS) */
static void emqx_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri    = EMQX_BROKER_URI,    /* mqtt://broker.emqx.io:1883 */
        .credentials.client_id = EMQX_CLIENT_ID,
        .session.last_will = {
            .topic = MQTT_TOPIC_STATUS, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 0,
        },
        .session.keepalive = 60,
    };
    s_emqx_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_emqx_client, ESP_EVENT_ANY_ID, emqx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_emqx_client));

    ESP_LOGI(TAG_MQTT, "Cliente emqx iniciado → %s", EMQX_BROKER_URI);
}

/* Publica el estado actual de sensores en JSON a ems/data */
/* Publica telemetría. emqx siempre (dashboard ágil); AWS solo si
 * include_aws (cada 10 s) para no exceder el free tier.            */
static void mqtt_publish_state(bool include_aws)
{
    char payload[180];  /* Buffer para el JSON de salida */

    /* JSON publicado: temperatura del DHT22, gas crudo, estado del extractor.
     * (Humedad y temp2 se omiten — no se usan en la maqueta final.) */
    int len = snprintf(payload, sizeof(payload),
        "{\"temp\":%.1f,\"gas\":%d,\"fan_state\":%d,\"fan_pwm\":%lu}",
        g_state.temp_dht,
        g_state.gas_raw,
        g_state.fan_on ? 1 : 0,
        (unsigned long) g_state.fan_pwm);

    if (len <= 0 || len >= (int) sizeof(payload)) {
        ESP_LOGE(TAG_MQTT, "Buffer JSON insuficiente");
        return;
    }

    /* emqx (dashboard): cada llamada. AWS (free tier): solo si include_aws */
    if (s_emqx_connected) {
        esp_mqtt_client_publish(s_emqx_client, MQTT_TOPIC_DATA, payload, len, 0, 0);
    }
    if (include_aws && s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_DATA, payload, len, 0, 0);
        ESP_LOGI(TAG_MQTT, "Publicado [%s] (aws+emqx): %s", MQTT_TOPIC_DATA, payload);
    } else {
        ESP_LOGD(TAG_MQTT, "Publicado emqx: %s", payload);
    }
}


/* ════════════════════════════════════════════════════════════
 *   TAREA FreeRTOS — lectura de sensores y control
 * ════════════════════════════════════════════════════════════ */

/* Tarea principal: lee sensores, aplica lógica ON/OFF y publica MQTT */
static void sensor_control_task(void *pvParameters)
{
    ESP_LOGI(TAG_MAIN, "Tarea de sensores iniciada");

    uint32_t cycle = 0;  /* Contador de ciclos para espaciar la publicación MQTT */

    while (1) {
        bool dht_failed = false;  /* true si el DHT22 falló este ciclo (modo maqueta) */
#if USE_NTC_SIM
        /* ── Simulación: NTC analógica en GPIO4 → temp2 ────────── */
        g_state.temp2 = adc_read_temp2();
        ESP_LOGI(TAG_ADC, "NTC temp2=%.1f", g_state.temp2);
#else
        /* ── Maqueta: DHT22 digital en GPIO4 → temp + humedad ──── */
        float dht_temp = 0.0f, hum = 0.0f;
        esp_err_t dht_err = dht22_read(&dht_temp, &hum);

        if (dht_err == ESP_OK) {
            g_state.temp_dht = dht_temp;  /* Temperatura DHT22 en °C  */
            g_state.humidity = hum;       /* Humedad relativa en %    */
            ESP_LOGI(TAG_DHT, "DHT22 Temp=%.1f°C  Hum=%.1f%%", dht_temp, hum);
        } else {
            ESP_LOGW(TAG_DHT, "Lectura fallida, conservando último valor");
            dht_failed = true;
        }
#endif

        /* ── Leer MQ-2 en ADC1_CH1 (GPIO1) ────────────────── */
        int gas = adc_read_gas();         /* Raw 0–4095            */
        g_state.gas_raw = gas;
        ESP_LOGI(TAG_ADC, "MQ-2 raw=%d", gas);

        /* ── Lógica ON/OFF con histéresis ──────────────────────── */
        if (g_emergency_stop) {
            /* Paro de emergencia: extractor forzado OFF, ignora control auto */
            if (g_state.fan_on || g_state.fan_pwm != 0) {
                g_state.fan_on  = false;
                g_state.fan_pwm = 0;
                fan_set_duty(0);
                ESP_LOGW(TAG_MAIN, "Extractor OFF por PARO DE EMERGENCIA");
            }
        } else if (!g_state.fan_on && gas > g_cfg.gas_on) {
            /* Gas superó umbral → encender extractor al 100% */
            g_state.fan_on  = true;
            g_state.fan_pwm = 1023;              /* Duty máximo (100%)   */
            fan_set_duty(g_state.fan_pwm);        /* Aplicar PWM          */
            ESP_LOGW(TAG_MAIN, "Extractor ON — gas=%d > umbral=%d",
                     gas, g_cfg.gas_on);
            shadow_report();                      /* Reflejar cambio en la sombra */

        } else if (g_state.fan_on && gas < g_cfg.gas_off) {
            /* Gas bajó del umbral inferior → apagar extractor */
            g_state.fan_on  = false;
            g_state.fan_pwm = 0;                 /* Duty 0% (apagado)    */
            fan_set_duty(0);                      /* Apagar PWM           */
            ESP_LOGI(TAG_MAIN, "Extractor OFF — gas=%d < umbral_off=%d",
                     gas, g_cfg.gas_off);
            shadow_report();                      /* Reflejar cambio en la sombra */
        }

        /* ── Lógica de alarma (gas + temperatura DHT22) ───────── */
        bool alarm = (gas > g_cfg.gas_on)
                  || (g_state.temp_dht > g_cfg.temp_alarm);
        g_state.alarm_active = alarm;
        alarm_set(alarm);  /* Rojo si alarma activa, verde si todo OK */

        /* ── Refresh de la pantalla OLED (temp + gas + estado FAN) ─
         * En modo NTC sim se grafica la temperatura del NTC; en modo
         * maqueta (DHT22 real) se grafica la temperatura del DHT22.
         * Si la última lectura del DHT22 falló se muestra error en lugar
         * del render normal.                                            */
        if (dht_failed) {
            oled_show_dht_error();
        } else {
#if USE_NTC_SIM
            oled_render(g_state.temp2,    g_state.gas_raw, alarm);
#else
            oled_render(g_state.temp_dht, g_state.gas_raw, alarm);
#endif
        }

        /* ── Publicar: emqx cada ciclo (1 s), AWS cada PUBLISH_EVERY_N (10 s) ── */
        cycle++;
        mqtt_publish_state(cycle % PUBLISH_EVERY_N == 0);

        /* ── Esperar hasta el próximo ciclo de control ─────────── */
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}


/* ════════════════════════════════════════════════════════════
 *   APP_MAIN — punto de entrada
 * ════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG_MAIN, "=== Estación de Monitoreo Ambiental — ESP32-C6 ===");

    /* ── Inicializar NVS (requerido por WiFi) ──────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());   /* Borrar NVS si está corrupto */
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_LOGI(TAG_MAIN, "NVS inicializado");

    /* ── Cargar configuración (WiFi, AWS, umbrales) desde NVS ── */
    app_config_load();

    /* ── Inicializar periféricos ────────────────────────────── */
    adc_init();        /* ADC1 para MQ-2          */
    ledc_fan_init();   /* PWM LEDC para extractor   */
    alarm_gpio_init(); /* LED rojo de alarma + LED verde OK */
    buttons_init();    /* Botón de reset de configuración */
    status_led_init(); /* LED RGB de estado (WS2812) */
    oled_init();                    /* Pantalla SSD1306 I2C */
    oled_show_splash();             /* Bienvenida: "Sistema iniciado" */

    /* ── Estado del LED RGB según la ruta de arranque ─────────
     * Si no hay credenciales arrancamos en portal cautivo (rojo lento).
     * Si hay credenciales pero todavía no hay enlace, mostramos
     * RECONNECTING (naranja rápido) hasta que el handler de WiFi
     * confirme IP (azul fijo).                                          */
    status_led_set(app_config_has_wifi() ? LED_ST_RECONNECTING : LED_ST_PORTAL);

    /* ── Portal cautivo si no hay credenciales guardadas ──
     * provisioning_start_blocking() no retorna: al guardar, reinicia
     * y ese arranque entrará por la rama STA normal.                  */
    if (!app_config_has_wifi()) {
        ESP_LOGW(TAG_MAIN, "Sin credenciales WiFi — iniciando portal de configuración");
        provisioning_start_blocking();
    }

    /* ── Arrancar las tareas ANTES de WiFi/MQTT ───────────────
     * Así la pantalla OLED y la lógica del extractor empiezan a refrescar
     * con las lecturas reales de los sensores desde el primer segundo,
     * sin tener que esperar a que terminen SNTP, TLS y MQTT (~5-30 s).  */
    xTaskCreate(sensor_control_task, "sensor_ctrl", 4096, NULL, 5, NULL);
    xTaskCreate(factory_button_task, "btn_factory", 2048, NULL, 4, NULL);

    /* ── Construir endpoints AWS con la config cargada ────────── */
    aws_build_endpoints();

    /* ── Conectar WiFi ──────────────────────────────────────── */
    esp_err_t wifi_ret = wifi_init_sta();
    if (wifi_ret != ESP_OK) {
        /* Sin WiFi la estación igual opera localmente */
        ESP_LOGW(TAG_MAIN, "Sin WiFi — operando en modo local");
    } else {
        /* ── Sincronizar hora (obligatorio para el handshake TLS) ── */
        time_sync_init();
        /* ── Cliente MQTT/TLS hacia AWS IoT (capa segura + sombra) ── */
        mqtt_init();
        /* ── Cliente MQTT hacia emqx (alimenta el dashboard web) ───── */
        emqx_init();
    }

    ESP_LOGI(TAG_MAIN, "Sistema iniciado — control cada %d ms, publicando cada %d ms",
             CONTROL_PERIOD_MS, CONTROL_PERIOD_MS * PUBLISH_EVERY_N);
}
