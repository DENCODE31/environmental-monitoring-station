/* ============================================================
 *   provisioning — portal cautivo de configuración WiFi
 * ============================================================
 *   Cuando el dispositivo no tiene credenciales WiFi guardadas,
 *   levanta un Access Point abierto ("EnvStation-XXXX") con:
 *     - Servidor DNS que secuestra todas las consultas → 192.168.4.1
 *       (dispara el popup de "iniciar sesión en la red" del celular).
 *     - Servidor HTTP con portal: escaneo de redes en vivo con nivel
 *       de señal, formulario WiFi y ajustes avanzados (AWS, umbrales).
 *     - mDNS → accesible también como http://estacion.local
 *   Al guardar, persiste en NVS y reinicia para entrar en modo STA.
 * ============================================================ */
#pragma once

/* Inicia el modo provisioning y bloquea indefinidamente. No retorna:
 * cuando el usuario guarda la configuración, el dispositivo se reinicia. */
void provisioning_start_blocking(void);
