/* ============================================================
 *   app_config — implementación
 * ============================================================ */
#include "app_config.h"
#include <string.h>
#include <stddef.h>                      /* offsetof para CRC32 sobre el blob */
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_rom_crc.h"                 /* esp_rom_crc32_le para checksum del blob NVS */
#include "secrets.h"                     /* AWS_IOT_ENDPOINT, AWS_THING_NAME, WIFI_AP_LIST */

#define CFG_NAMESPACE "ems_cfg"          /* Espacio de nombres NVS dedicado a la config */
#define CFG_KEY       "blob"             /* Clave única: toda la config en un blob       */
#define CFG_MAGIC     0x454D5303u        /* 'EMS' + versión 03 (con gas_baseline) */

/* Calcula el CRC32 LE sobre todos los campos del struct excepto el propio
 * campo crc32 (que va al final). Si la struct cambia de tamaño en disco
 * vs RAM, el cálculo seguirá siendo coherente porque usamos offsetof. */
static uint32_t cfg_compute_crc(const app_config_t *c)
{
    const size_t payload_len = offsetof(app_config_t, crc32);
    return esp_rom_crc32_le(0, (const uint8_t *) c, payload_len);
}

/* Valores por defecto si NVS está vacío (primer arranque o tras reset).
 * gas_baseline=0 dispara auto-calibración al arrancar. gas_on/gas_off se
 * calculan como baseline + delta a menos que el usuario los sobreescriba. */
#define DEF_TEMP_ALARM 35.0f

static const char *TAG = "CFG";

app_config_t g_cfg;

/* Carga valores de fábrica en g_cfg (no persiste). Sin redes WiFi: el
 * equipo arranca sin credenciales y se configuran por el portal cautivo.
 * Endpoint/thing de AWS sí se siembran desde secrets.h como valores base. */
static void cfg_set_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic       = CFG_MAGIC;
    g_cfg.ap_count    = 0;                 /* Sin redes → portal en el primer arranque */
    g_cfg.gas_baseline = 0;                /* 0 → dispara auto-calibración en main */
    g_cfg.gas_on      = 1800;              /* Valor temporal, se recalibrará */
    g_cfg.gas_off     = 1400;
    g_cfg.temp_alarm  = DEF_TEMP_ALARM;
    strlcpy(g_cfg.aws_endpoint, AWS_IOT_ENDPOINT, sizeof(g_cfg.aws_endpoint));
    strlcpy(g_cfg.aws_thing,    AWS_THING_NAME,   sizeof(g_cfg.aws_thing));
}

void app_config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        cfg_set_defaults();
        return;
    }

    size_t sz = sizeof(g_cfg);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &g_cfg, &sz);
    nvs_close(h);

    if (err != ESP_OK || sz != sizeof(g_cfg) || g_cfg.magic != CFG_MAGIC) {
        ESP_LOGW(TAG, "Config NVS ausente/incompatible — usando defaults");
        cfg_set_defaults();
        return;
    }

    /* Validación de integridad: el blob pudo haber quedado corrupto si
     * hubo corte de luz durante un nvs_set_blob anterior. */
    uint32_t calc = cfg_compute_crc(&g_cfg);
    if (calc != g_cfg.crc32) {
        ESP_LOGE(TAG, "Config NVS con CRC inválido (calc=0x%08lx blob=0x%08lx) — usando defaults",
                 (unsigned long) calc, (unsigned long) g_cfg.crc32);
        cfg_set_defaults();
        return;
    }

    ESP_LOGI(TAG, "Config cargada: %d red(es), endpoint=%s, thing=%s, gas_on/off=%d/%d, temp_alarm=%.1f",
             g_cfg.ap_count, g_cfg.aws_endpoint, g_cfg.aws_thing,
             g_cfg.gas_on, g_cfg.gas_off, g_cfg.temp_alarm);
}

bool app_config_has_wifi(void)
{
    return g_cfg.ap_count > 0;
}

esp_err_t app_config_save(void)
{
    g_cfg.magic = CFG_MAGIC;
    g_cfg.crc32 = cfg_compute_crc(&g_cfg);  /* Sellar antes de escribir */

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CFG_KEY, &g_cfg, sizeof(g_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_add_wifi(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    const char *p = pass ? pass : "";

    /* Si el SSID ya existe, solo actualiza la contraseña */
    for (int i = 0; i < g_cfg.ap_count; i++) {
        if (strcmp(g_cfg.aps[i].ssid, ssid) == 0) {
            strlcpy(g_cfg.aps[i].pass, p, CFG_PASS_LEN);
            return app_config_save();
        }
    }

    /* Lista llena → descartar la más antigua (corrimiento hacia el inicio) */
    if (g_cfg.ap_count >= CFG_MAX_APS) {
        for (int i = 1; i < CFG_MAX_APS; i++) {
            g_cfg.aps[i - 1] = g_cfg.aps[i];
        }
        g_cfg.ap_count = CFG_MAX_APS - 1;
    }

    strlcpy(g_cfg.aps[g_cfg.ap_count].ssid, ssid, CFG_SSID_LEN);
    strlcpy(g_cfg.aps[g_cfg.ap_count].pass, p,    CFG_PASS_LEN);
    g_cfg.ap_count++;
    return app_config_save();
}

void app_config_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, CFG_KEY);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "Config NVS borrada (factory reset)");
    }
    /* Limpiar también las redes en RAM: deja el equipo sin credenciales */
    g_cfg.ap_count = 0;
    memset(g_cfg.aps, 0, sizeof(g_cfg.aps));
}

void app_config_recalibrate(int baseline)
{
    g_cfg.gas_baseline = baseline;
    g_cfg.gas_on  = baseline + CAL_GAS_DELTA_ON;
    g_cfg.gas_off = baseline + CAL_GAS_DELTA_OFF;
    app_config_save();
    ESP_LOGW(TAG, "Recalibrado MQ-2: baseline=%d on=%d off=%d",
             baseline, g_cfg.gas_on, g_cfg.gas_off);
}
