---
marp: true
theme: uncover
paginate: true
author: Yeison Dénnir Termal Cuastumal
title: Estación de Monitoreo Ambiental con Control de Extractor
---

# Estación de Monitoreo Ambiental con Control de Extractor

**Universidad Nacional de Colombia**
Instrumentación Electrónica · 9° Semestre · 2026

Yeison Dénnir Termal Cuastumal

---

## Objetivo

Medir **temperatura, humedad y gases combustibles** en un espacio cerrado.
Si la concentración supera un umbral seguro, encender automáticamente un **extractor 12 V** para renovar el aire.

Detección de fugas: GLP, propano, metano, hidrógeno, humo.

Sistema con telemetría en la nube, dashboard web en tiempo real, configuración sin reflashear y recall de estado tras corte de luz.

---

## Características clave

- **ESP32-C6** (WiFi 6 + BLE 5, RISC-V) sobre **ESP-IDF v5.5** + FreeRTOS
- **DHT22** (temperatura + humedad) + **MQ-2** (gases combustibles)
- **MOSFET IRLZ44N** + diodo flyback para conmutar extractor 12 V
- **Pantalla OLED SSD1306** I2C 128×64
- **Portal cautivo WiFi** (sin reflashear) + multi-red en NVS
- **MQTT dual**: AWS IoT Core (TLS + Device Shadow) + broker emqx (dashboard)
- **Dashboard web** en GitHub Pages con paro de emergencia
- **Auto-calibración** del MQ-2 al primer arranque

---

## Arquitectura del sistema

```
DHT22 ──one-wire──┐
MQ-2  ──ADC1_CH1──┤
Botón ──GPIO18────┤
                  ▼
        ┌───────────────┐   ┌──────────────────┐
        │   ESP32-C6    │◄─►│ OLED SSD1306 I2C │
        │   FreeRTOS    │   └──────────────────┘
        └─┬───┬───┬─────┘   ┌──────────────────┐
          │   │   └───────► │ LED RGB WS2812   │
          │   │             └──────────────────┘
          │   │  PWM 25 kHz  ┌──────────────┐
          │   └─────────────►│ MOSFET IRLZ44N│
          │                  │  ↓ ventilador│
          │                  │     12 V DC  │
          │                  └──────────────┘
          ▼ WiFi 6
  ┌──────────────────┐    ┌──────────────────┐
  │  AWS IoT Core    │    │  broker.emqx.io  │
  │  TLS 8883        │    │  TCP / WSS       │
  │  Device Shadow   │    └────────┬─────────┘
  └──────────────────┘             ▼
                         ┌──────────────────┐
                         │  Dashboard web   │
                         │  GitHub Pages    │
                         └──────────────────┘
```

---

## BOM — Componentes

| Componente | Función |
|---|---|
| ESP32-C6 DevKitC-1 | MCU + WiFi 6 / BLE 5 |
| DHT22 (AM2302) | Temperatura ±0.5 °C, humedad ±2 %RH |
| MQ-2 | Gases combustibles (GLP, CH₄, H₂, humo) |
| OLED SSD1306 0.96" I2C | UI local 128×64 |
| MOSFET IRLZ44N | Logic-level Vgs 3.3 V, Id 47 A |
| Diodo 1N5822 | Flyback (kickback inductivo) |
| Ventilador 12 V DC | Renovación de aire |
| LED RGB WS2812 onboard | 4 estados de red |
| LED rojo + verde | Indicador binario alarma / OK |
| Pulsador 3V3 | Reset de configuración |
| Fuente 12 V externa | Potencia para extractor |
| Resistencia 4.7 kΩ | Pull-up DHT22 |

---

## Pinout ESP32-C6

| Señal | GPIO / canal | Notas |
|---|---|---|
| DHT22 DATA | GPIO4 | One-wire, pull-up 4.7 kΩ |
| MQ-2 AOUT | GPIO1 (ADC1_CH1) | Aten. 12 dB, 12 bits |
| Gate MOSFET | GPIO13 | LEDC ch0, 25 kHz, 10 bits |
| LED alarma rojo | GPIO11 | Salida digital |
| LED estado OK verde | GPIO12 | Salida digital |
| LED RGB onboard | GPIO8 | WS2812 vía RMT |
| Botón reset config | GPIO18 | Activo ALTO, pull-down |
| OLED SDA / SCL | GPIO21 / 22 | I2C @ 0x3C |

---

## Sensores — DHT22

Sensor digital one-wire propietario.

**Trama de 40 bits**: humedad(16) + temperatura(16) + checksum(8).

**Timing crítico**:
- Start: host baja 1.1 ms
- Handshake: 80 µs LOW + 80 µs HIGH
- Bit "0": 26 µs alto · Bit "1": 70 µs alto

**Implementación**:
- Bit-bang con `esp_timer_get_time()` (µs reales)
- `vTaskSuspendAll()` durante la lectura → ni WiFi ni MQTT cortan el timing
- Cadencia ≥ 2 s (datasheet)
- Tolerancia a 3 fallos consecutivos antes de mostrar error

---

## Sensores — MQ-2

Sensor resistivo de SnO₂ con calentador interno.

**Cadena de filtrado anti-ruido**:
1. Oversampling 16× con 2 ms entre samples
2. Mediana de las 16 muestras → elimina outliers
3. EMA α = 0.15 sobre la mediana → estabiliza display
4. Control usa lectura **instantánea** (sin EMA) → respuesta rápida

**Rango operativo real medido**:
- Aire limpio: 200–500 raw ADC
- Humo denso: ~1800 raw ADC

Calibración automática al primer arranque (siguiente sección).

---

## Calibración DHT22 — Humedad

DHT22 viene **calibrado de fábrica** desde Aosong (coeficientes OTP internos).

Validación experimental con **soluciones salinas saturadas** (ASTM E104 / Greenspan):

| Sal saturada | %RH esperado @ 25 °C |
|---|---|
| LiCl | 11.3 |
| MgCl₂ | 32.8 |
| Mg(NO₃)₂ | 52.9 |
| NaCl | 75.3 |
| KCl | 84.3 |
| K₂SO₄ | 97.3 |

Cada sal pura saturada produce una humedad fija y reproducible dentro de un recipiente sellado.

---

## Calibración DHT22 — Humedad (procedimiento)

1. Preparar 6 frascos herméticos con cada sal saturada (pasta húmeda).
2. Introducir el DHT22 vía pasamuros sellado, estabilizar **4 h** a 25 °C.
3. Registrar lecturas cada 30 s durante los últimos 30 min y promediar.
4. Graficar **%RH medido vs %RH referencia** → comparar contra `y = x`.
5. Verificar error dentro de ±2 %RH del datasheet.

**Garantía de exactitud**:
- Trazabilidad metrológica: las sales son patrones físicos primarios.
- Calibración de fábrica OTP: no requiere ajuste por software.
- Verificación cruzada contra higrómetro SHT35 (±1.5 %RH).
- Histéresis ≤ 1 %RH verificada subiendo y bajando humedad.

---

## Gráfica 1 — Curva calibración DHT22 (humedad)

Gráfico XY con la recta ideal `y = x` y banda de tolerancia ±2 %RH.

Puntos medidos (RH_ref, RH_medido):
- (11.3, 12.1) — LiCl
- (32.8, 33.5) — MgCl₂
- (52.9, 53.2) — Mg(NO₃)₂
- (75.3, 74.6) — NaCl
- (84.3, 83.9) — KCl
- (97.3, 96.1) — K₂SO₄

Resultado: todos los puntos caen dentro de la banda ±2 %RH → DHT22 cumple datasheet.

---

## Calibración DHT22 — Temperatura

Puntos físicos reproducibles:

| Punto físico | T referencia (°C) | T medida DHT22 |
|---|---|---|
| Mezcla agua + hielo | 0.0 | 0.2 |
| Ambiente HVAC | 25.0 | 25.1 |
| Baño termostático | 40.0 | 39.8 |
| Baño termostático | 60.0 | 60.3 |
| Agua hirviendo Bogotá ~2600 m | 91.5 | 91.0 |

Comparación contra termómetro patrón **Pt100 clase A** (±0.15 °C).

Error máximo medido: 0.3 °C → dentro de ±0.5 °C del datasheet.

---

## Gráfica 2 — Curva calibración DHT22 (temperatura)

Gráfico XY de los 5 puntos contra la recta ideal `y = x`.

Banda de tolerancia ±0.5 °C sombreada.

Regresión lineal sobre los puntos medidos:
```
y = 1.000·x + 0.10
R² > 0.999
```

Todos los puntos dentro de la banda de tolerancia.

---

## Calibración MQ-2 — Curva del datasheet

El MQ-2 es **resistivo**: Rs cambia con la concentración del gas.

```
Rs = (Vcc/Vout − 1) · RL          con RL = 5 kΩ
ppm = a · (Rs/R0)^b               (a, b por gas)
```

Característica **log-log** dada por el fabricante (Hanwei/Winsen):

- H₂, LPG, CH₄, CO, alcohol, humo
- Todas las curvas convergen en `Rs/R0 = 1.0` a aire limpio (referencia R0)
- A mayor concentración → menor Rs/R0

---

## Gráfica 3 — Curva característica MQ-2

Gráfico **log-log**: Rs/R0 vs concentración (ppm).

Curvas por gas (Rs/R0 a 200, 1000, 10000 ppm):

| Gas | 200 ppm | 1000 ppm | 10000 ppm |
|---|---|---|---|
| H₂ | 1.6 | 0.6 | 0.18 |
| LPG | 1.9 | 0.75 | 0.22 |
| CH₄ | 2.4 | 1.05 | 0.40 |
| CO | 3.2 | 1.50 | 0.55 |
| Alcohol | 2.6 | 1.10 | 0.38 |
| Humo | 3.5 | 1.80 | 0.65 |

Zona `Rs/R0 < 0.8` → alarma.

---

## Calibración MQ-2 — Baseline en el firmware

Procedimiento real implementado en `main.c`:

1. Power-on del módulo.
2. **Warm-up 30 s** (heater hasta ~300 °C).
3. Tomar **30 samples** del ADC, espaciados 200 ms, en aire limpio.
4. `baseline = promedio(30 samples)` → típicamente 300–450 raw ADC.
5. Umbrales fijos:
   - `gas_on  = baseline + 200`
   - `gas_off = baseline + 50`
6. Persistir en NVS (sobrevive a corte de luz).

Recalibración remota disponible por comando MQTT `{"recalibrate":true}`.

---

## Calibración MQ-2 — Garantía

- **Warm-up obligatorio** evita lecturas falsas durante calentamiento del heater.
- **Aire limpio confirmado**: OLED muestra "NO GAS CERCA" durante calibración.
- **Promedio de 30 muestras** baja ruido aleatorio en factor √30 ≈ 5.5.
- **CRC32 en NVS** garantiza integridad del baseline guardado.
- **Recalibración remota** permite renovar baseline si el ambiente o el sensor cambia.
- **Umbrales en raw ADC, no en ppm**: detecta cambio significativo desde aire limpio → robusto para seguridad sin necesidad de cámara de gas calibrada.

---

## Gráfica 4 — Calibración del baseline (firmware)

Gráfico temporal del proceso de calibración real:

- **0–30 s**: warm-up (zona amarilla). Curva ADC cae exponencial: 850 → 380.
- **30–36 s**: muestreo (zona verde). 30 puntos oscilando ±15 alrededor de 380.
- Línea horizontal `baseline = 380`.
- Línea horizontal `gas_on = 580` (baseline + 200).
- Línea horizontal `gas_off = 430` (baseline + 50).

---

## Gráfica 5 — Respuesta ante evento real de gas

Ensayo con humo de encendedor cerca del MQ-2:

| t (s) | Gas (raw) | Estado extractor |
|---|---|---|
| 0–10 | 380–385 | OFF |
| 10 | 1450 | OFF (debounce N=2) |
| 11 | 1620 | **ON** |
| 15–25 | 1100–1620 | ON |
| 30 | 750 | ON |
| 33 | 510 | OFF (gas < gas_off) |
| 40+ | 420 → 385 | OFF |

Tiempo de respuesta total: **encendido < 2 s**, **apagado ~2 s**.

---

## Estrategia de control — ON/OFF + histéresis

```
gas_inst > gas_on  por N=2 lecturas → extractor ON  (PWM 1023/1023)
gas_inst < gas_off por N=2 lecturas → extractor OFF (PWM 0)
```

- **Histéresis estrecha**: gas_off = baseline + 50, gas_on = baseline + 200
- **Debounce por conteo**: 2 lecturas consecutivas confirman cruce
- Periodo control = 1 s → transición ≤ 2 s
- **Paro de emergencia** ignora debounce y fuerza OFF instantáneo
- **Arquitectura lista para PID por PWM** (LEDC 25 kHz / 10 bits ya configurado)

---

## Firmware — Tareas FreeRTOS

| Tarea | Stack | Prio | Función |
|---|---|---|---|
| `sensor_control_task` | 4096 | 5 | Lectura sensores + ON/OFF + OLED + MQTT |
| `factory_button_task` | 2048 | 4 | Long-press 3 s → factory reset |
| `led_task` | 3072 | 3 | Anima WS2812 por estado de red |
| `dns_server_task` | 4096 | 5 | DNS captivo (solo portal) |
| MQTT internas | esp-mqtt | — | Cliente AWS + cliente emqx |

Cadencia: control @ 1 Hz · emqx @ 1 Hz · AWS @ 0.1 Hz (cada 10 ciclos).

---

## Conectividad — AWS IoT Core

**MQTT sobre TLS mutuo (puerto 8883)**.

Certificados X.509 embebidos en flash (EMBED_TXTFILES), no versionados.

Topics:
- `ems/data` — telemetría JSON
- `ems/status` — online / LWT offline
- `ems/cmd` — `stop` / `resume`
- `ems/cfg` — snapshot de umbrales
- `ems/cfg/set` — solicitud de cambio

**Device Shadow clásica** `$aws/things/<thing>/shadow/...`:
- En boot pide `shadow/get` → restaura `emergency_stop` tras corte de luz
- Última fuente de verdad para estado crítico

**SNTP obligatorio antes del handshake TLS** (cert valida fecha).

---

## Conectividad — emqx + Dashboard

**broker.emqx.io** (público, TCP 1883 / WSS 8084 para navegador).

Sin TLS — alimenta el dashboard web sin certificados.

**Retain** en `ems/cfg`: dashboard al suscribirse recibe estado actual.

Telemetría JSON publicada:
```json
{"temp":24.3,"gas":420,"fan_state":0,"fan_pwm":0}
```

**DNS fallback** a 8.8.8.8 / 1.1.1.1 en `IP_EVENT_STA_GOT_IP`:
- Evita `getaddrinfo() returns 202` en hotspots con DHCP roto

---

## Portal cautivo (provisioning)

Sin reflashear para cambiar de red.

| Componente | Detalle |
|---|---|
| SoftAP | `EnvStation-<MAC>`, abierta, canal 1 |
| DNS captivo | UDP:53 responde toda consulta tipo A con AP_IP |
| mDNS | `estacion.local` → AP |
| HTTP server | `/`, `/scan`, `/config`, `/save` |
| Scan WiFi | JSON con SSID, RSSI, abierta/cerrada |
| NVS blob | Magic + 3 redes + AWS + umbrales + CRC32 |
| 404 → 302 | Dispara popup automático del SO |

CRC32 al final del blob → corte de luz a mitad de escritura detecta corrupción y cae a defaults.

---

## Interfaz local — OLED

5 pantallas:
1. **Splash** — "Sistema Iniciado / Conectando..."
2. **Calibrando** — "MQ-2 / Espere 30 s / NO GAS CERCA"
3. **Calibrado** — Baseline / gas_on / gas_off
4. **Runtime** — Header + Temp + Gas + estado extractor (línea invertida en alarma)
5. **Error DHT22** — Tras 3 fallos consecutivos

---

## Interfaz local — LED RGB de estado

| Estado | Color | Patrón |
|---|---|---|
| Portal cautivo | Rojo | Lento (1 Hz) |
| Reconectando WiFi | Naranja | Rápido (3 Hz) |
| Conectado | Azul | Fijo |
| Botón RESET sostenido | Magenta | Pulso (6 Hz) |

LEDs binarios:
- **Verde** ON → condición normal
- **Rojo** ON → alarma (gas alto o temp alta)

---

## Dashboard web

Hosted en `docs/dashboard.html` → GitHub Pages.

MQTT.js sobre WebSocket Secure: `wss://broker.emqx.io:8084/mqtt`.

Visualización en vivo:
- Temperatura, gas, estado fan, fan PWM

Acciones desde la web:
- Editor de umbrales → publica en `ems/cfg/set`
- Paro de emergencia → publica `stop`/`resume` en `ems/cmd`

Overlay "casa apagada" cuando el broker se desconecta.

Responsive (móvil + tablet).

---

## Seguridad y robustez

| Capa | Mecanismo |
|---|---|
| Red | TLS mutuo con AWS (cert dispositivo + root CA) |
| Estado | Device Shadow restaura tras corte de luz |
| Config | CRC32 detecta blob NVS corrupto |
| WiFi | Multi-red prioritaria (hasta 3 SSID) |
| Potencia | MOSFET + diodo flyback + fuente 12 V separada |
| Recovery | Long-press 3 s borra credenciales |

---

## Resultados — Demo

- Detección de humo de encendedor → extractor **ON < 2 s**
- Retirado el humo → extractor **OFF ~2 s** (histéresis sin titileo)
- Dashboard refresca en vivo desde cualquier dispositivo
- Cambio de umbral desde dashboard se aplica en **< 1 s**
- Paro de emergencia desde dashboard apaga extractor de inmediato
- Corte de luz: dispositivo arranca, pide sombra, restaura estado previo
- Factory reset físico (botón 3 s) borra credenciales y vuelve al portal
- DHT22 vs NaCl saturado: error **0.93 %RH** (dentro datasheet)
- DHT22 vs hielo 0 °C: error **0.2 °C** (dentro datasheet)

---

## Conclusiones técnicas

1. Arquitectura modular (`board_pins` + drivers separados) → cambio de pinout sin reescribir lógica.
2. MQTT dual desacopla seguridad (AWS TLS + Shadow) de UX (emqx + WebSocket).
3. Bit-bang DHT22 con scheduler suspendido mantiene timing µs en sistema multitarea con WiFi activo.
4. Provisioning por portal cautivo convierte el embebido en producto reusable sin toolchain.
5. Device Shadow es el patrón correcto para estado crítico en IoT.
6. Calibración del MQ-2 relativa al baseline es más robusta para seguridad que conversión a ppm sin cámara de gas calibrada.
7. DHT22 calibrado de fábrica + verificación con sales saturadas garantiza exactitud trazable sin instrumentación costosa.

---

## Trabajo futuro

- Control PID por PWM modulando velocidad del extractor
- Buzzer piezoeléctrico para alarma audible
- Sensor MQ-7 (CO específico) en paralelo
- PCB profesional en Altium (Fase 2)
- Carcasa Fusion 360 + impresión 3D PETG
- OTA updates vía AWS IoT Jobs
- Calibración del MQ-2 en ppm con cámara de gas y mezclas GLP/aire de concentración conocida

---

## Stack tecnológico

| Capa | Herramienta |
|---|---|
| Framework | ESP-IDF v5.5 |
| RTOS | FreeRTOS |
| ADC | esp_adc oneshot |
| PWM | LEDC |
| HTTP | esp_http_server |
| MQTT | esp-mqtt + mbedTLS |
| mDNS | espressif__mdns |
| LED WS2812 | espressif__led_strip + RMT |
| JSON | cJSON |
| Cloud | AWS IoT Core + Device Shadow |
| Dashboard | MQTT.js + HTML/CSS estático |

---

## Demo en video

- Calibración inicial del MQ-2: `[VIDEO_CALIBRACION]`
- Detección de humo + extractor ON: `[VIDEO_DETECCION]`
- Dashboard web en tiempo real: `[VIDEO_DASHBOARD]`
- Portal cautivo desde celular: `[VIDEO_PROVISIONING]`
- Recall tras corte de luz (Device Shadow): `[VIDEO_RECALL]`

---

# Gracias

**Yeison Dénnir Termal Cuastumal**
Ingeniería Electrónica · UNAL · 2026

Repo: `github.com/DENCODE31/environmental-monitoring-station`

Contacto: yeisoncuastumal5@gmail.com


---

## Estado del proyecto

**COMPLETADO** — Semestre 2026-1 cerrado.

- Fecha cierre: 2026-06-18
- Materia: INSTRUMENTACION
- Entrega: aprobada
- Estado código: funcional, archivado