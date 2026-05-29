/* ============================================================
 *   provisioning — implementación
 * ============================================================ */
#include "provisioning.h"
#include "app_config.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "PROV";

#define AP_IP        "192.168.4.1"   /* IP fija del SoftAP (default de ESP-IDF) */
#define AP_CHANNEL   1
#define AP_MAX_CONN  4
#define DNS_PORT     53
#define SCAN_MAX_AP  20              /* Redes máximas a listar en el portal     */
#define GAS_ADC_MAX  4095            /* Fondo de escala ADC 12-bit (mapea % humo)*/
#define GAS_HYST_PCT 85              /* Umbral de apagado = 85% del de encendido */

static httpd_handle_t s_httpd = NULL;

/* ════════════════════════════════════════════════════════════
 *   PÁGINA WEB DEL PORTAL — servida desde flash como string
 * ════════════════════════════════════════════════════════════ */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>EnvStation — Configuración</title><style>"
"*{box-sizing:border-box}body{margin:0;font-family:system-ui,Segoe UI,Roboto,sans-serif;"
"background:#0f172a;color:#e2e8f0;display:flex;justify-content:center;padding:18px}"
".card{width:100%;max-width:420px;background:#1e293b;border-radius:16px;padding:22px;"
"box-shadow:0 10px 30px rgba(0,0,0,.4)}h1{font-size:20px;margin:0 0 4px}"
".sub{color:#94a3b8;font-size:13px;margin-bottom:18px}label{font-size:13px;color:#cbd5e1;"
"display:block;margin:14px 0 6px}input,select{width:100%;padding:11px;border-radius:9px;"
"border:1px solid #334155;background:#0f172a;color:#e2e8f0;font-size:15px}"
"input[type=range]{padding:0;height:30px;accent-color:#22d3ee}"
"button{width:100%;margin-top:20px;padding:13px;border:0;border-radius:9px;font-size:16px;"
"font-weight:600;background:#22d3ee;color:#062a30;cursor:pointer}button:disabled{opacity:.5}"
".row{display:flex;gap:10px}.row>div{flex:1}.bars{font-family:monospace;color:#22d3ee}"
".adv{margin-top:14px;border-top:1px solid #334155;padding-top:6px}summary{cursor:pointer;"
"color:#94a3b8;font-size:13px}.msg{margin-top:14px;font-size:14px;text-align:center;min-height:18px}"
".ok{color:#4ade80}.err{color:#f87171}small{color:#64748b}</style></head><body><div class=\"card\">"
"<h1>Estación Ambiental</h1><div class=\"sub\">Configura la red WiFi para conectar el equipo.</div>"
"<label>Red WiFi</label>"
"<div class=\"row\"><div><select id=\"ssid\"></select></div>"
"<div style=\"flex:0 0 70px\"><button style=\"margin:0;padding:11px\" id=\"rescan\" type=\"button\">⟳</button></div></div>"
"<input id=\"ssidManual\" placeholder=\"o escribe el SSID manualmente\" style=\"margin-top:8px\">"
"<label>Contraseña</label><input id=\"pass\" type=\"password\" placeholder=\"contraseña WiFi\">"
"<details class=\"adv\"><summary>Ajustes avanzados (AWS y umbrales)</summary>"
"<label>AWS IoT endpoint</label><input id=\"ep\">"
"<label>Thing name</label><input id=\"th\">"
"<label>Umbral de temperatura (°C)</label>"
"<input id=\"ta\" type=\"number\" min=\"0\" max=\"80\" step=\"1\">"
"<label>Nivel de humo para activar el ventilador: <b id=\"gasv\">50</b>%</label>"
"<input id=\"gas\" type=\"range\" min=\"0\" max=\"100\" step=\"1\" value=\"50\">"
"</details>"
"<button id=\"save\">Guardar y conectar</button>"
"<div class=\"msg\" id=\"msg\"></div>"
"<small>EnvStation · provisioning</small></div><script>"
"const $=i=>document.getElementById(i);"
"function bars(r){let n=r>-55?4:r>-65?3:r>-75?2:1;return '\\u2588'.repeat(n)+'\\u2591'.repeat(4-n)}"
"async function scan(){$('ssid').innerHTML='<option>Buscando...</option>';"
"try{let r=await fetch('/scan');let l=await r.json();$('ssid').innerHTML='';"
"l.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{let o=document.createElement('option');"
"o.value=n.ssid;o.textContent=(n.open?'\\uD83D\\uDD13 ':'\\uD83D\\uDD12 ')+n.ssid+'  '+bars(n.rssi);"
"$('ssid').appendChild(o)})}catch(e){$('ssid').innerHTML='<option>Error al escanear</option>'}}"
"async function loadCfg(){try{let c=await(await fetch('/config')).json();"
"$('ep').value=c.aws_endpoint;$('th').value=c.aws_thing;$('ta').value=c.temp_alarm;"
"let p=Math.round(c.gas_on/4095*100);$('gas').value=p;$('gasv').textContent=p}catch(e){}}"
"$('gas').oninput=()=>{$('gasv').textContent=$('gas').value};"
"$('rescan').onclick=scan;"
"$('save').onclick=async()=>{let ssid=$('ssidManual').value.trim()||$('ssid').value;"
"if(!ssid){$('msg').className='msg err';$('msg').textContent='Selecciona una red';return}"
"$('save').disabled=true;$('msg').className='msg';$('msg').textContent='Guardando...';"
"let body={ssid:ssid,pass:$('pass').value,aws_endpoint:$('ep').value,aws_thing:$('th').value,"
"temp_alarm:+$('ta').value,gas_pct:+$('gas').value};"
"try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(body)});if(!r.ok)throw 0;$('msg').className='msg ok';"
"$('msg').textContent='\\u2713 Guardado. Reiniciando y conectando...'}"
"catch(e){$('save').disabled=false;$('msg').className='msg err';$('msg').textContent='Error al guardar'}};"
"scan();loadCfg();</script></body></html>";

/* ════════════════════════════════════════════════════════════
 *   SERVIDOR DNS CAPTIVO — responde toda consulta A con AP_IP
 * ════════════════════════════════════════════════════════════ */

/* Cabecera DNS de 12 bytes (RFC 1035) */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

/* Registro de respuesta tipo A, con puntero comprimido al nombre */
typedef struct __attribute__((packed)) {
    uint16_t name_ptr;   /* 0xC00C → apunta al nombre en la pregunta */
    uint16_t type;       /* 1 = A                                    */
    uint16_t cls;        /* 1 = IN                                   */
    uint32_t ttl;
    uint16_t rd_len;     /* 4 bytes de dirección IPv4                */
    uint32_t ip;         /* IP en orden de red                       */
} dns_answer_t;

/* Convierte la consulta recibida en una respuesta DNS.
 *   - Consulta tipo A (IPv4)  → responde con 'ip'.
 *   - Otra consulta (AAAA...) → responde NOERROR sin registros, para que
 *     el cliente no espere timeout y caiga de inmediato a IPv4.
 * Devuelve el nuevo tamaño del paquete, o 0 si no aplica. */
static int dns_build_reply(uint8_t *buf, int len, uint32_t ip)
{
    if (len < (int) sizeof(dns_header_t)) return 0;
    dns_header_t *hdr = (dns_header_t *) buf;
    if (ntohs(hdr->qd_count) != 1) return 0;       /* Solo manejamos 1 pregunta */

    /* Saltar el nombre de la pregunta (labels terminadas en 0x00) */
    int p = sizeof(dns_header_t);
    while (p < len && buf[p] != 0) {
        p += buf[p] + 1;
    }
    if (p + 4 >= len) return 0;                    /* Paquete malformado */
    uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];   /* Tipo de consulta */
    p += 1 + 4;                                    /* byte nulo + qtype(2) + qclass(2) */

    hdr->flags    = htons(0x8180);                 /* Respuesta estándar, sin error */
    hdr->ns_count = 0;
    hdr->ar_count = 0;

    if (qtype != 1) {                              /* No es A → respuesta vacía rápida */
        hdr->an_count = 0;
        return p;                                  /* Cabecera + pregunta, sin answer */
    }

    if (p + (int) sizeof(dns_answer_t) > 512) return 0;
    hdr->an_count = htons(1);

    dns_answer_t *ans = (dns_answer_t *) (buf + p);
    ans->name_ptr = htons(0xC00C);                 /* Puntero al offset 12 (nombre) */
    ans->type     = htons(1);
    ans->cls      = htons(1);
    ans->ttl      = htonl(60);
    ans->rd_len   = htons(4);
    ans->ip       = ip;                            /* Ya en orden de red */
    return p + sizeof(dns_answer_t);
}

/* Tarea: escucha UDP:53 y responde toda consulta con la IP del AP */
static void dns_server_task(void *pv)
{
    uint8_t buf[512];
    uint32_t ap_ip = inet_addr(AP_IP);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: no se pudo crear socket");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "DNS: bind falló en puerto %d", DNS_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS captivo escuchando en UDP:%d", DNS_PORT);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *) &src, &sl);
        if (n <= 0) continue;
        int out = dns_build_reply(buf, n, ap_ip);
        if (out > 0) {
            sendto(sock, buf, out, 0, (struct sockaddr *) &src, sl);
        }
    }
}

/* ════════════════════════════════════════════════════════════
 *   HANDLERS HTTP
 * ════════════════════════════════════════════════════════════ */

/* GET / → portal HTML */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* GET /scan → JSON con las redes detectadas y su nivel de señal */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);                /* Escaneo bloqueante */

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > SCAN_MAX_AP) num = SCAN_MAX_AP;

    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    uint16_t got = num;
    esp_wifi_scan_get_ap_records(&got, recs);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < got; i++) {
        if (recs[i].ssid[0] == '\0') continue;     /* Omitir redes ocultas/sin nombre */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char *) recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o,   "open", recs[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);

    char *txt = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "[]");
    free(txt);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* GET /config → valores actuales para precargar el formulario avanzado */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "aws_endpoint", g_cfg.aws_endpoint);
    cJSON_AddStringToObject(o, "aws_thing",    g_cfg.aws_thing);
    cJSON_AddNumberToObject(o, "gas_on",       g_cfg.gas_on);
    cJSON_AddNumberToObject(o, "gas_off",      g_cfg.gas_off);
    cJSON_AddNumberToObject(o, "temp_alarm",   g_cfg.temp_alarm);

    char *txt = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
    cJSON_Delete(o);
    return ESP_OK;
}

/* POST /save → persiste configuración y reinicia */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[512];
    int max = req->content_len < (int) sizeof(buf) - 1
            ? req->content_len : (int) sizeof(buf) - 1;
    int r = httpd_req_recv(req, buf, max);
    if (r <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return ESP_FAIL;
    }

    cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(jssid) || jssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID requerido");
        return ESP_FAIL;
    }
    cJSON *jpass = cJSON_GetObjectItem(root, "pass");
    cJSON *jep   = cJSON_GetObjectItem(root, "aws_endpoint");
    cJSON *jth   = cJSON_GetObjectItem(root, "aws_thing");
    cJSON *jta   = cJSON_GetObjectItem(root, "temp_alarm");
    cJSON *jgp   = cJSON_GetObjectItem(root, "gas_pct");

    /* Ajustes avanzados: solo se sobrescriben si vienen con valor válido */
    if (cJSON_IsString(jep) && jep->valuestring[0])
        strlcpy(g_cfg.aws_endpoint, jep->valuestring, CFG_ENDPOINT_LEN);
    if (cJSON_IsString(jth) && jth->valuestring[0])
        strlcpy(g_cfg.aws_thing, jth->valuestring, CFG_THING_LEN);
    if (cJSON_IsNumber(jta) && jta->valuedouble > 0)
        g_cfg.temp_alarm = (float) jta->valuedouble;

    /* Nivel de humo en % (0–100) → ADC raw; el apagado es histéresis automática */
    if (cJSON_IsNumber(jgp)) {
        int pct = jgp->valueint;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        g_cfg.gas_on  = pct * GAS_ADC_MAX / 100;
        g_cfg.gas_off = g_cfg.gas_on * GAS_HYST_PCT / 100;
    }

    esp_err_t err = app_config_add_wifi(jssid->valuestring,
                                        cJSON_IsString(jpass) ? jpass->valuestring : "");
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ESP_LOGW(TAG, "Configuración guardada — reiniciando en modo STA");
    vTaskDelay(pdMS_TO_TICKS(1500));               /* Dar tiempo a entregar la respuesta */
    esp_restart();
    return ESP_OK;
}

/* Cualquier otra URL (probes de detección de portal) → redirige a la raíz.
 * Junto con el DNS captivo, dispara el popup automático del sistema. */
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Registra el servidor HTTP y sus rutas */
static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor HTTP");
        return;
    }

    httpd_uri_t root   = { .uri = "/",       .method = HTTP_GET,  .handler = root_get_handler   };
    httpd_uri_t scan   = { .uri = "/scan",   .method = HTTP_GET,  .handler = scan_get_handler   };
    httpd_uri_t conf   = { .uri = "/config", .method = HTTP_GET,  .handler = config_get_handler };
    httpd_uri_t save   = { .uri = "/save",   .method = HTTP_POST, .handler = save_post_handler  };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &conf);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_redirect);

    ESP_LOGI(TAG, "Servidor HTTP del portal iniciado");
}

/* ════════════════════════════════════════════════════════════
 *   ARRANQUE DEL MODO PROVISIONING
 * ════════════════════════════════════════════════════════════ */

void provisioning_start_blocking(void)
{
    /* Stack de red + loop de eventos (no inicializados aún en esta rama) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();           /* STA habilitado para poder escanear */

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));

    /* SSID único por dispositivo usando los 2 últimos bytes de la MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "EnvStation-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *) ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len       = strlen(ssid);
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode       = WIFI_AUTH_OPEN;         /* Red abierta: fácil de unir */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "═══════════════════════════════════════════════");
    ESP_LOGW(TAG, " MODO PROVISIONING ACTIVO");
    ESP_LOGW(TAG, " 1. Conéctate a la red WiFi: '%s'", ssid);
    ESP_LOGW(TAG, " 2. Abre http://estacion.local  (o http://%s)", AP_IP);
    ESP_LOGW(TAG, "═══════════════════════════════════════════════");

    /* mDNS → http://estacion.local resuelve al AP */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("estacion");
        mdns_instance_name_set("EnvStation Setup");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    /* DNS captivo + servidor HTTP */
    xTaskCreate(dns_server_task, "dns_captive", 4096, NULL, 5, NULL);
    start_http_server();

    /* Bloquear aquí: el guardado de credenciales dispara esp_restart() */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
