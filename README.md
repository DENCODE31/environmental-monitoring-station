![License](https://img.shields.io/badge/License-MIT-blue.svg)
![Status](https://img.shields.io/badge/Status-Development-yellow.svg)
![ESP32-C6](https://img.shields.io/badge/MCU-ESP32--C6-red.svg)
![Made in Colombia](https://img.shields.io/badge/Made%20in-Colombia-yellow.svg)

# Estación de Monitoreo Ambiental con Control de Extractor

**Universidad Nacional de Colombia · Instrumentación Electrónica · 9° Semestre**

Sistema embebido sobre **ESP32-C6** que mide temperatura, humedad y calidad de aire (gases) y acciona un extractor cuando la concentración de gas supera un umbral. El control parte de una estrategia **ON/OFF con histéresis** y deja la arquitectura lista para control **PID por PWM** sobre la velocidad del extractor.

Incluye **aprovisionamiento WiFi por portal cautivo** (sin reflashear para cambiar de red), telemetría a **AWS IoT Core** (MQTT sobre TLS + Device Shadow) y un **dashboard web** alimentado por un broker MQTT público. Desarrollado con **ESP-IDF** y **FreeRTOS**.

---

## Características

- **Portal cautivo de configuración** — el dispositivo levanta un Access Point, escanea las redes disponibles (con nivel de señal), y permite ingresar credenciales WiFi y umbrales desde el navegador. Todo se guarda en NVS: **no hace falta reflashear** ni editar código para cambiar de red.
- **Configuración persistente en NVS** — redes WiFi (hasta 3, con prioridad), endpoint/thing de AWS y umbrales de temperatura y humo.
- **Botones de reconfiguración** — botón de arranque (entra al portal) y botón de reset por pulsación larga (borra credenciales y reinicia al portal).
- **LED RGB de estado** — rojo parpadeando mientras configura/reconecta, azul fijo al conectar.
- **Telemetría dual** — AWS IoT Core (TLS + Device Shadow para recordar estado tras corte de luz) y broker emqx público para el dashboard en tiempo real.
- **Control ON/OFF con histéresis** sobre el extractor, con arquitectura PWM (LEDC) lista para PID.
- **Alarma** — LED rojo + buzzer al superar el umbral crítico de gas o temperatura.

---

## Estructura del repositorio

```text
environmental-monitoring-station/
├── main/
│   ├── main.c              # app_main, WiFi/STA, MQTT, control y alarma
│   ├── app_config.c/.h     # Configuración persistente en NVS
│   ├── provisioning.c/.h    # Portal cautivo (SoftAP + DNS + HTTP + mDNS)
│   ├── status_led.c/.h     # LED RGB de estado (WS2812)
│   ├── secrets.h.example   # Plantilla de credenciales (copiar a secrets.h)
│   ├── idf_component.yml   # Dependencias gestionadas (mdns, led_strip)
│   └── CMakeLists.txt
├── docs/                   # Dashboard web (GitHub Pages)
│   ├── dashboard.html      # Panel en tiempo real (MQTT sobre WebSocket)
│   └── index.html          # Redirección al dashboard
├── sdkconfig.defaults      # Configuración reproducible (target, flash, particiones)
├── CMakeLists.txt
├── .devcontainer/          # Entorno reproducible
├── .vscode/                # Configuración del editor
└── .clangd
```

---

## Flujo de funcionamiento

```text
Arranque → carga config (NVS)
  ├── Botón GPIO2 presionado → borra credenciales → PORTAL
  ├── Sin credenciales         → PORTAL
  └── Con credenciales          → STA → AWS IoT + emqx → tarea de sensores

Portal cautivo (red "EnvStation-XXXX", http://estacion.local):
  escanear redes → elegir → contraseña → (avanzado: AWS, umbrales)
  → guardar en NVS → reiniciar → conecta solo

En operación: mantener botón GPIO18 por 3 s → borra credenciales → portal
```

---

## Componentes de hardware

| Categoría | Componente | Función |
|-----------|-----------|---------|
| Sensor ambiental | DHT22 | Temperatura + humedad relativa (one-wire) |
| Sensor de gases | MQ-135 | Calidad de aire: NH₃, NOx, alcohol, benceno, humo, CO₂ aprox. |
| Actuador | Ventilador / extractor DC 12 V | Renovación de aire bajo alarma |
| Driver de potencia | MOSFET IRLZ44N + diodo flyback | Conmutación del extractor (lógica 3.3 V, carga 12 V) |
| Indicadores | LED RGB onboard, LED alarma, buzzer | Estado de red y alarma |
| Botones | 2 pulsadores a GND | Provisioning y reset de configuración |
| Embebido | ESP32-C6 | Control + conectividad (WiFi 6 / BLE 5) |

---

## Pinout (ESP32-C6)

| Señal | GPIO | Notas |
|-------|------|-------|
| DHT22 DATA / NTC | GPIO4 | One-wire (maqueta) o NTC por ADC1_CH4 (simulación) |
| MQ-135 AOUT | GPIO1 | ADC1_CH1 |
| Extractor (gate MOSFET) | GPIO13 | PWM (LEDC) / ON-OFF |
| LED alarma | GPIO11 | LED rojo |
| Buzzer | GPIO12 | Activo por nivel |
| LED RGB de estado | GPIO8 | WS2812 onboard (DevKitC-1) |
| Botón provisioning | GPIO2 | A GND, pull-up interno. Presionado al arranque → portal |
| Botón reset config | GPIO18 | A GND, pull-up interno. 3 s en operación → portal |

---

## Diagrama de conexiones

![Diagrama de conexiones](docs/diagrama_conexiones.png)

> Fuente vectorial editable: [`docs/diagrama_conexiones.svg`](docs/diagrama_conexiones.svg)

---

## Estrategia de control

| Modo | Descripción |
|------|-------------|
| ON/OFF + histéresis | Si el gas supera el umbral de encendido, el extractor arranca al 100 %; se apaga al bajar del umbral inferior (85 % del de encendido). Evita el titileo. Mínimo viable. |
| PID por PWM | (Fase 2) Control modulando la velocidad del extractor vía LEDC según concentración o temperatura. |

Los umbrales (temperatura en °C y nivel de humo en %) se configuran desde el portal cautivo.

---

## Stack tecnológico

| Herramienta | Uso |
|-------------|-----|
| ESP-IDF v5.5 | Framework principal |
| FreeRTOS | Multitarea |
| esp_adc | Lectura del MQ-135 / NTC |
| LEDC | PWM del extractor |
| esp_http_server + mdns | Portal cautivo |
| esp-mqtt + TLS | Telemetría a AWS IoT Core y emqx |
| led_strip | LED RGB de estado |

---

## Compilar y flashear

Requiere [ESP-IDF v5.5+](https://docs.espressif.com/projects/esp-idf/).

```bash
# 1. Credenciales y certificados (no versionados)
cp main/secrets.h.example main/secrets.h     # editar endpoint/thing de AWS
# Colocar los certificados X.509 en main/certs/:
#   aws_root_ca.pem, aws_device_cert.pem, aws_device_key.pem

# 2. Target (flash y particiones vienen de sdkconfig.defaults)
idf.py set-target esp32c6

# 3. Compilar, flashear y monitorear
idf.py -p COM<X> flash monitor
```

Primer arranque: el equipo crea la red **EnvStation-XXXX**. Conéctate desde el celular, abre `http://estacion.local` y configura tu red WiFi.

Para salir del monitor: `Ctrl + ]`

---

## Autor

**Yeison Dénnir Termal Cuastumal**  
Ingeniería Electrónica — Universidad Nacional de Colombia · 2026  
[GitHub](https://github.com/DENCODE31)
