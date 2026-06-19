/* ============================================================
 *   app_config — configuración persistente en NVS
 * ============================================================
 *   Centraliza toda la configuración modificable en campo:
 *   redes WiFi conocidas, endpoint/thing de AWS IoT y umbrales
 *   de control. Se guarda como un único blob en NVS, de modo
 *   que el dispositivo nunca necesita reflashearse para cambiar
 *   credenciales o parámetros. Los valores por defecto se siembran
 *   desde secrets.h en el primer arranque (NVS vacío).
 * ============================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CFG_MAX_APS      3      /* Máximo de redes WiFi recordadas        */
#define CAL_GAS_DELTA_ON  200   /* gas_on  = baseline + este delta        */
#define CAL_GAS_DELTA_OFF  50   /* gas_off = baseline + este delta        */
#define CAL_N_SAMPLES     30   /* Número de lecturas para promediar baseline */
#define CFG_SSID_LEN     33     /* 32 chars + null                        */
#define CFG_PASS_LEN     65     /* 64 chars + null                        */
#define CFG_ENDPOINT_LEN 128    /* Endpoint AWS IoT (ats...amazonaws.com) */
#define CFG_THING_LEN    64     /* Nombre del objeto (thing name)         */

/* Una red WiFi conocida: SSID + contraseña */
typedef struct {
    char ssid[CFG_SSID_LEN];
    char pass[CFG_PASS_LEN];
} cfg_ap_t;

/* Configuración completa del dispositivo (se serializa como blob NVS).
 * El campo `crc32` debe ser SIEMPRE el último: se calcula sobre todos
 * los bytes anteriores. Si la escritura se interrumpe por corte de luz
 * y el blob queda parcialmente escrito, el CRC no coincidirá y el load
 * caerá en defaults en vez de leer datos corruptos.                    */
typedef struct {
    uint32_t magic;                       /* Marca de versión del blob              */
    int      ap_count;                    /* Redes guardadas (0..CFG_MAX_APS)        */
    cfg_ap_t aps[CFG_MAX_APS];            /* Lista de redes en orden de prioridad    */
    char     aws_endpoint[CFG_ENDPOINT_LEN];
    char     aws_thing[CFG_THING_LEN];
    int      gas_on;                      /* Umbral ADC: enciende extractor          */
    int      gas_off;                     /* Umbral ADC: apaga (histéresis)          */
    int      gas_baseline;                /* Nivel de aire limpio (0=no calibrado)   */
    float    temp_alarm;                  /* Temperatura crítica de alarma (°C)      */
    uint32_t crc32;                       /* Checksum de integridad — debe ir último */
} app_config_t;

/* Instancia global cargada en RAM. Las tareas leen de aquí. */
extern app_config_t g_cfg;

/* Carga la configuración desde NVS hacia g_cfg. Si no existe o es
 * incompatible, siembra valores por defecto (sin persistir todavía). */
void app_config_load(void);

/* true si hay al menos una red WiFi guardada (define si arranca en STA
 * o en modo provisioning). */
bool app_config_has_wifi(void);

/* Agrega o actualiza una red WiFi en la lista y persiste todo el blob.
 * Si el SSID ya existe, actualiza su contraseña; si la lista está llena,
 * descarta la más antigua. */
esp_err_t app_config_add_wifi(const char *ssid, const char *pass);

/* Persiste g_cfg completo en NVS (WiFi + AWS + umbrales). */
esp_err_t app_config_save(void);

/* Borra la configuración de NVS (vuelve a provisioning en el próximo arranque). */
void app_config_factory_reset(void);

/* Recalibra baseline del MQ-2: fija baseline, actualiza gas_on/gas_off y persiste. */
void app_config_recalibrate(int baseline);
