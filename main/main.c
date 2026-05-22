/* ============================================================
 *   Estación de Monitoreo Ambiental — Control de Extractor
 * ============================================================
 *   Lee temperatura/humedad (DHT22) y calidad de aire (MQ-135).
 *   Acciona extractor 12V vía MOSFET cuando gas > umbral.
 *   Publica datos cada 10 s por MQTT TCP a broker.emqx.io.
 *   Muestra estado en OLED SSD1306 e indica alarma con LED+buzzer.
 *   Arquitectura lista para PID por PWM (LEDC) en fase 2.
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "rom/ets_sys.h"
#include "secrets.h"                      /* WIFI_SSID y WIFI_PASS (no versionado) */

/* ── Credenciales WiFi ─────────────────────────────────────── */
/* WIFI_SSID y WIFI_PASS se definen en secrets.h (ver secrets.h.example) */
#define WIFI_MAX_RETRY 10                 /* Intentos antes de reiniciar */

/* ── MQTT ──────────────────────────────────────────────────── */
#define MQTT_BROKER_URI  "mqtt://broker.emqx.io:1883"  /* Broker público gratuito */
#define MQTT_TOPIC_DATA  "ems/data"                    /* Publicación de sensores  */
#define MQTT_TOPIC_STATUS "ems/status"                 /* LWT online/offline       */
#define MQTT_TOPIC_CMD   "ems/cmd"                     /* Comandos entrantes        */
#define MQTT_CLIENT_ID   "esp32c6_ems_01"             /* ID único en el broker    */
#define MQTT_PUBLISH_MS  1000                          /* Intervalo de publicación (ms) */

/* ── Pines GPIO ────────────────────────────────────────────── */
#define GPIO_DHT22       GPIO_NUM_4    /* One-wire DHT22, pull-up 4.7 kΩ */
#define GPIO_FAN_PWM     GPIO_NUM_13   /* Gate MOSFET IRLZ44N (LEDC) — LED de prueba */
#define GPIO_LED_ALARM   GPIO_NUM_11   /* LED rojo de alarma              */
#define GPIO_BUZZER      GPIO_NUM_12   /* Buzzer activo                   */

/* ── ADC: canales 1 y 4 de ADC1 ───────────────────────────── */
/* ESP32-C6: ADC1_CH1 = GPIO1 | ADC1_CH4 = GPIO4               */
#define ADC_CH_GAS       ADC_CHANNEL_1  /* GPIO1 — MQ-135 (calidad de aire)  */
#define ADC_CH_TEMP      ADC_CHANNEL_4  /* GPIO4 — LM35 (temperatura analógica) */

/* Conversión LM35: Vout = 10 mV/°C con Vref = 3.3 V y ADC 12 bits
 * Temp(°C) = (raw / 4095.0) * 3300 / 10                        */
#define LM35_ADC_TO_CELSIUS(raw)  ((float)(raw) * 3300.0f / 4095.0f / 10.0f)

/* ── LEDC (PWM extractor) ──────────────────────────────────── */
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL     LEDC_CHANNEL_0
#define LEDC_DUTY_RES    LEDC_TIMER_10_BIT  /* Resolución: 0–1023     */
#define LEDC_FREQ_HZ     25000              /* 25 kHz, inaudible      */

/* ── Umbrales de alarma ────────────────────────────────────── */
#define GAS_THRESHOLD_ON   1800   /* ADC raw — enciende extractor */
#define GAS_THRESHOLD_OFF  1400   /* Histéresis — apaga extractor */
#define TEMP_ALARM_C       35.0f  /* Temperatura crítica (°C)     */

/* ── DHT22: timing (µs) ────────────────────────────────────── */
#define DHT22_START_LOW_US   1100  /* Pulso inicio host → sensor  */
#define DHT22_RESP_WAIT_US   40    /* Espera respuesta del sensor  */
#define DHT22_BIT_TIMEOUT_US 100   /* Timeout por bit              */

/* ── Tags de log ───────────────────────────────────────────── */
static const char *TAG_MAIN  = "EMS";
static const char *TAG_WIFI  = "WIFI";
static const char *TAG_MQTT  = "MQTT";
static const char *TAG_DHT   = "DHT22";
static const char *TAG_ADC   = "ADC";

/* ── Estado global compartido entre tareas ─────────────────── */
typedef struct {
    float    humidity;       /* Humedad relativa en % — DHT22         */
    float    temp_dht;       /* Temperatura °C — DHT22 (digital)      */
    float    temp_lm35;      /* Temperatura °C — LM35 ADC1_CH4 (GPIO4) */
    int      gas_raw;        /* ADC raw — MQ-135 ADC1_CH1 (GPIO1)     */
    bool     fan_on;         /* Estado del extractor                  */
    uint32_t fan_pwm;        /* Ciclo de trabajo 0–1023               */
    bool     alarm_active;   /* Bandera de alarma                     */
} sensor_state_t;

static sensor_state_t g_state = {0};  /* Estado global inicializado a cero */

static volatile bool g_emergency_stop = false;  /* Paro de emergencia: fuerza extractor OFF */

/* ── Handles de FreeRTOS ───────────────────────────────────── */
static EventGroupHandle_t  s_wifi_event_group;  /* Grupo de eventos WiFi */
static esp_mqtt_client_handle_t s_mqtt_client;  /* Handle del cliente MQTT */

/* ── ADC ───────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle;  /* Handle del ADC oneshot */

/* Bits de evento WiFi */
#define WIFI_CONNECTED_BIT BIT0  /* Set cuando conectado */
#define WIFI_FAIL_BIT      BIT1  /* Set cuando falla definitivamente */

static int s_retry_num = 0;  /* Contador de reintentos WiFi */


/* ════════════════════════════════════════════════════════════
 *   DRIVER DHT22 — bit-bang sobre GPIO4
 * ════════════════════════════════════════════════════════════ */

/* Espera activa hasta que el pin alcance 'level' o vence el timeout.
 * Retorna µs transcurridos, o -1 si hubo timeout. */
static int dht22_wait_level(int level, int timeout_us)
{
    int elapsed = 0;
    /* Sondeo cada ~1 µs hasta detectar el nivel esperado */
    while (gpio_get_level(GPIO_DHT22) != level) {
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
    gpio_set_direction(GPIO_DHT22, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_DHT22, 0);
    ets_delay_us(DHT22_START_LOW_US);           /* Mantiene bajo 1.1 ms */
    gpio_set_level(GPIO_DHT22, 1);
    ets_delay_us(30);                           /* Sube y espera 30 µs  */
    gpio_set_direction(GPIO_DHT22, GPIO_MODE_INPUT);

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


/* ════════════════════════════════════════════════════════════
 *   ADC — MQ-135 en ADC1_CH0 (GPIO0)
 * ════════════════════════════════════════════════════════════ */

/* Inicializa ADC1: CH1 = MQ-135 (GPIO1), CH4 = LM35 temperatura (GPIO4) */
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

    /* Canal 1 (GPIO1) — MQ-135: calidad de aire / gases */
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        s_adc_handle, ADC_CH_GAS, &chan_config));

    /* Canal 4 (GPIO4) — LM35: temperatura analógica (10 mV/°C) */
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        s_adc_handle, ADC_CH_TEMP, &chan_config));

    ESP_LOGI(TAG_ADC, "ADC1 CH1 (GPIO1, MQ-135) y CH4 (GPIO4, LM35) inicializados");
}

/* Lee el MQ-135 en ADC1_CH1 (GPIO1). Retorna raw 0–4095. */
static int adc_read_gas(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CH_GAS, &raw));   /* Lectura MQ-135 */
    ESP_LOGD(TAG_ADC, "MQ-135 CH1 raw=%d", raw);
    return raw;
}

/* Lee el LM35 en ADC1_CH4 (GPIO4). Retorna temperatura en °C. */
static float adc_read_temp_lm35(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CH_TEMP, &raw));  /* Lectura LM35 */
    float temp_c = LM35_ADC_TO_CELSIUS(raw);                             /* Convertir a °C */
    ESP_LOGD(TAG_ADC, "LM35 CH4 raw=%d → %.1f°C", raw, temp_c);
    return temp_c;
}


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
        .gpio_num   = GPIO_FAN_PWM,    /* GPIO10 → Gate MOSFET     */
        .duty       = 0,               /* Arranca en 0% (apagado)  */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));  /* Aplicar config del canal */

    ESP_LOGI(TAG_MAIN, "LEDC fan PWM inicializado (GPIO%d, %d Hz)", GPIO_FAN_PWM, LEDC_FREQ_HZ);
}

/* Establece el ciclo de trabajo del extractor (0–1023) */
static void fan_set_duty(uint32_t duty)
{
    duty = duty > 1023 ? 1023 : duty;              /* Clamp al máximo     */
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);  /* Aplicar duty cycle  */
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);     /* Refrescar hardware  */
}


/* ════════════════════════════════════════════════════════════
 *   GPIO — LED de alarma y buzzer
 * ════════════════════════════════════════════════════════════ */

/* Configura LED y buzzer como salidas digitales */
static void alarm_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_LED_ALARM) | (1ULL << GPIO_BUZZER), /* Ambos pines */
        .mode         = GPIO_MODE_OUTPUT,      /* Salida digital          */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));  /* Aplicar configuración */

    gpio_set_level(GPIO_LED_ALARM, 0);  /* LED apagado al inicio  */
    gpio_set_level(GPIO_BUZZER, 0);     /* Buzzer apagado al inicio */

    ESP_LOGI(TAG_MAIN, "GPIO LED (GPIO%d) y buzzer (GPIO%d) inicializados",
             GPIO_LED_ALARM, GPIO_BUZZER);
}

/* Activa o desactiva la alarma visual y sonora */
static void alarm_set(bool active)
{
    gpio_set_level(GPIO_LED_ALARM, active ? 1 : 0);  /* LED rojo ON/OFF  */
    gpio_set_level(GPIO_BUZZER,    active ? 1 : 0);  /* Buzzer activo ON/OFF */
}


/* ════════════════════════════════════════════════════════════
 *   WIFI — conexión con reconexión automática
 * ════════════════════════════════════════════════════════════ */

/* Handler de eventos WiFi e IP */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();  /* Iniciar conexión al arrancar en modo STA */

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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
        s_retry_num = 0;                                              /* Reset contador */
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

    /* Configurar credenciales WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  /* Mínimo WPA2 */
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));       /* Modo estación */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());                        /* Arrancar WiFi  */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));           /* Sin modem sleep → MQTT sin latencia */

    ESP_LOGI(TAG_WIFI, "Conectando a '%s'...", WIFI_SSID);

    /* Esperar a conectar o fallar */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));  /* Timeout 30 s */

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "Conectado a WiFi: %s", WIFI_SSID);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG_WIFI, "No se pudo conectar a WiFi");
        return ESP_FAIL;
    }
}


/* ════════════════════════════════════════════════════════════
 *   MQTT — cliente con LWT y reconexión
 * ════════════════════════════════════════════════════════════ */

/* Handler de eventos del cliente MQTT */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "Conectado al broker MQTT");
            /* Publicar mensaje de presencia al conectar */
            esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS,
                                    "online", 6, 1, 1);  /* QoS 1, retain */
            /* Suscribirse al topic de comandos (paro de emergencia) */
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_MQTT, "Desconectado del broker MQTT");
            break;

        case MQTT_EVENT_DATA:
            /* Comando entrante: "stop" activa paro, "resume" reanuda control auto */
            if (event->topic_len == (int) strlen(MQTT_TOPIC_CMD) &&
                strncmp(event->topic, MQTT_TOPIC_CMD, event->topic_len) == 0) {
                if (event->data_len == 4 && strncmp(event->data, "stop", 4) == 0) {
                    g_emergency_stop = true;
                    ESP_LOGW(TAG_MQTT, "PARO DE EMERGENCIA activado");
                } else if (event->data_len == 6 && strncmp(event->data, "resume", 6) == 0) {
                    g_emergency_stop = false;
                    ESP_LOGI(TAG_MQTT, "Paro de emergencia liberado");
                }
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
    /* LWT: publicar "offline" si el dispositivo se desconecta inesperadamente */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri     = MQTT_BROKER_URI,   /* URI del broker          */
        .credentials.client_id  = MQTT_CLIENT_ID,    /* ID único del cliente    */
        .session.last_will = {
            .topic   = MQTT_TOPIC_STATUS,            /* Topic del LWT           */
            .msg     = "offline",                    /* Payload del LWT         */
            .msg_len = 7,
            .qos     = 1,                            /* QoS 1 para LWT          */
            .retain  = 1,                            /* Retener último mensaje  */
        },
        .session.keepalive = 60,                     /* Keepalive cada 60 s     */
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);  /* Crear cliente MQTT */
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(   /* Registrar handler de eventos */
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));  /* Iniciar cliente */

    ESP_LOGI(TAG_MQTT, "Cliente MQTT iniciado → %s", MQTT_BROKER_URI);
}

/* Publica el estado actual de sensores en JSON a ems/data */
static void mqtt_publish_state(void)
{
    char payload[160];  /* Buffer para el JSON de salida */

    /* JSON con ambas temperaturas, humedad, gas, extractor
     * temp_dht  : DHT22 digital (°C)
     * temp_lm35 : LM35 analógico ADC_CH4 (°C)
     * hum       : DHT22 (%)
     * gas       : MQ-135 ADC_CH1 (raw)                             */
    int len = snprintf(payload, sizeof(payload),
        "{\"temp\":%.1f,\"temp2\":%.1f,\"hum\":%.1f,\"gas\":%d,\"fan_state\":%d,\"fan_pwm\":%lu}",
        g_state.temp_dht,
        g_state.temp_lm35,
        g_state.humidity,
        g_state.gas_raw,
        g_state.fan_on ? 1 : 0,
        (unsigned long) g_state.fan_pwm);

    if (len > 0 && len < (int) sizeof(payload)) {
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_DATA,
                                             payload, len, 0, 0);  /* QoS 0, no retain */
        ESP_LOGI(TAG_MQTT, "Publicado [%s] msg_id=%d: %s",
                 MQTT_TOPIC_DATA, msg_id, payload);
    } else {
        ESP_LOGE(TAG_MQTT, "Buffer JSON insuficiente");
    }
}


/* ════════════════════════════════════════════════════════════
 *   TAREA FreeRTOS — lectura de sensores y control
 * ════════════════════════════════════════════════════════════ */

/* Tarea principal: lee sensores, aplica lógica ON/OFF y publica MQTT */
static void sensor_control_task(void *pvParameters)
{
    ESP_LOGI(TAG_MAIN, "Tarea de sensores iniciada");

    while (1) {
        /* ── Leer DHT22 (temperatura digital + humedad) ────────── */
        float dht_temp = 0.0f, hum = 0.0f;
        esp_err_t dht_err = dht22_read(&dht_temp, &hum);

        if (dht_err == ESP_OK) {
            g_state.temp_dht = dht_temp;  /* Temperatura DHT22 en °C  */
            g_state.humidity = hum;       /* Humedad relativa en %    */
            ESP_LOGI(TAG_DHT, "DHT22 Temp=%.1f°C  Hum=%.1f%%", dht_temp, hum);
        } else {
            ESP_LOGW(TAG_DHT, "Lectura fallida, conservando último valor");
        }

        /* ── Leer MQ-135 en ADC1_CH1 (GPIO1) ────────────────── */
        int gas = adc_read_gas();         /* Raw 0–4095            */
        g_state.gas_raw = gas;
        ESP_LOGI(TAG_ADC, "MQ-135 raw=%d", gas);

        /* ── Leer LM35 en ADC1_CH4 (GPIO4) ─────────────────── */
        float lm35_temp = adc_read_temp_lm35();   /* Temperatura en °C */
        g_state.temp_lm35 = lm35_temp;
        ESP_LOGI(TAG_ADC, "LM35 temp=%.1f°C", lm35_temp);

        /* ── Lógica ON/OFF con histéresis ──────────────────────── */
        if (g_emergency_stop) {
            /* Paro de emergencia: extractor forzado OFF, ignora control auto */
            if (g_state.fan_on || g_state.fan_pwm != 0) {
                g_state.fan_on  = false;
                g_state.fan_pwm = 0;
                fan_set_duty(0);
                ESP_LOGW(TAG_MAIN, "Extractor OFF por PARO DE EMERGENCIA");
            }
        } else if (!g_state.fan_on && gas > GAS_THRESHOLD_ON) {
            /* Gas superó umbral → encender extractor al 100% */
            g_state.fan_on  = true;
            g_state.fan_pwm = 1023;              /* Duty máximo (100%)   */
            fan_set_duty(g_state.fan_pwm);        /* Aplicar PWM          */
            ESP_LOGW(TAG_MAIN, "Extractor ON — gas=%d > umbral=%d",
                     gas, GAS_THRESHOLD_ON);

        } else if (g_state.fan_on && gas < GAS_THRESHOLD_OFF) {
            /* Gas bajó del umbral inferior → apagar extractor */
            g_state.fan_on  = false;
            g_state.fan_pwm = 0;                 /* Duty 0% (apagado)    */
            fan_set_duty(0);                      /* Apagar PWM           */
            ESP_LOGI(TAG_MAIN, "Extractor OFF — gas=%d < umbral_off=%d",
                     gas, GAS_THRESHOLD_OFF);
        }

        /* ── Lógica de alarma (gas + temperatura DHT22 o LM35) ── */
        bool alarm = (gas > GAS_THRESHOLD_ON)
                  || (g_state.temp_dht  > TEMP_ALARM_C)
                  || (g_state.temp_lm35 > TEMP_ALARM_C);
        g_state.alarm_active = alarm;
        alarm_set(alarm);  /* Actualizar LED y buzzer según estado de alarma */

        /* ── Publicar por MQTT ─────────────────────────────────── */
        mqtt_publish_state();

        /* ── Esperar hasta el próximo ciclo ────────────────────── */
        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_MS));
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

    /* ── Inicializar periféricos ────────────────────────────── */
    adc_init();        /* ADC1 para MQ-135          */
    ledc_fan_init();   /* PWM LEDC para extractor   */
    alarm_gpio_init(); /* LED rojo + buzzer activo  */

    /* ── Conectar WiFi ──────────────────────────────────────── */
    esp_err_t wifi_ret = wifi_init_sta();
    if (wifi_ret != ESP_OK) {
        /* Sin WiFi la estación igual opera localmente */
        ESP_LOGW(TAG_MAIN, "Sin WiFi — operando en modo local");
    } else {
        /* ── Iniciar cliente MQTT solo si hay WiFi ──────────── */
        mqtt_init();
        vTaskDelay(pdMS_TO_TICKS(1000));  /* Dar tiempo al broker para confirmar */
    }

    /* ── Crear tarea de sensores y control ─────────────────── */
    xTaskCreate(
        sensor_control_task,   /* Función de la tarea     */
        "sensor_ctrl",         /* Nombre (debug)          */
        4096,                  /* Stack en bytes          */
        NULL,                  /* Sin parámetros          */
        5,                     /* Prioridad media         */
        NULL                   /* Sin handle externo      */
    );

    ESP_LOGI(TAG_MAIN, "Sistema iniciado — publicando cada %d ms", MQTT_PUBLISH_MS);
}
