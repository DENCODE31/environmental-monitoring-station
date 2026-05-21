![License](https://img.shields.io/badge/License-MIT-blue.svg)
![Status](https://img.shields.io/badge/Status-Development-yellow.svg)
![ESP32-C6](https://img.shields.io/badge/MCU-ESP32--C6-red.svg)
![Made in Colombia](https://img.shields.io/badge/Made%20in-Colombia-yellow.svg)

# Estación de Monitoreo Ambiental con Control de Extractor

**Universidad Nacional de Colombia · Instrumentación Electrónica · 9° Semestre**

Sistema embebido sobre **ESP32-C6** que mide temperatura, humedad y calidad de aire (gases), y acciona un extractor cuando la concentración de gas supera un umbral. El control parte de una estrategia **ON/OFF** y deja la arquitectura lista para control **PID por PWM** sobre la velocidad del extractor. Desarrollado con el framework **ESP-IDF** y **FreeRTOS**, con visualización local en pantalla OLED e indicadores de alarma.

---

## Estructura del repositorio

```
environmental-monitoring-station/
├── main/
│   ├── main.c              # Punto de entrada (app_main) y lógica de control
│   └── CMakeLists.txt      # Registro del componente principal
├── CMakeLists.txt          # Proyecto ESP-IDF
├── .devcontainer/          # Entorno reproducible (ESP-IDF + QEMU)
├── .vscode/                # Configuración del editor (IntelliSense, OpenOCD)
└── .clangd                 # Flags del language server
```

---

## Descripción funcional

El dispositivo ejecuta de forma periódica:

1. **Adquisición** — lectura de temperatura y humedad (DHT22) y de concentración de gas (MQ-135 por ADC).
2. **Decisión** — comparación contra umbrales. Si el gas supera el límite, se activa el extractor.
3. **Actuación** — control del extractor 12 V mediante MOSFET de potencia (ON/OFF, o PWM en la versión PID).
4. **Alarma** — LED rojo + buzzer al superar el umbral crítico.
5. **Visualización** — lecturas y estado del sistema en pantalla OLED y por UART.
6. **(Opcional)** — envío de telemetría por WiFi (MQTT) aprovechando la conectividad del ESP32-C6.

---

## Componentes

| Categoría | Componente | Función |
|-----------|-----------|---------|
| Sensor ambiental | DHT22 | Temperatura + humedad relativa (protocolo one-wire) |
| Sensor de gases | MQ-135 | Calidad de aire: NH₃, NOx, alcohol, benceno, humo, CO₂ aprox. |
| Actuador | Ventilador / extractor DC 12 V | Renovación de aire bajo condición de alarma |
| Driver de potencia | MOSFET IRLZ44N + diodo flyback | Conmutación del extractor (lógica 3.3 V, 12 V de carga) |
| Pantalla | OLED SSD1306 0.96" I2C 128×64 | Interfaz local de lectura |
| Indicadores | LED estado, LED alarma, buzzer | Señalización visual y audible |
| Embebido | ESP32-C6 | Control + conectividad (WiFi 6 / BLE 5) |

---

## Pinout propuesto (ESP32-C6)

> Preliminar — se fija definitivamente al cerrar el bring-up de hardware.

| Señal | GPIO | Notas |
|-------|------|-------|
| DHT22 DATA | GPIO4 | One-wire + pull-up 4.7 kΩ |
| MQ-135 AOUT | GPIO0 (ADC1_CH0) | Divisor de tensión a rango ADC seguro |
| OLED SDA / SCL | GPIO5 / GPIO6 | Bus I2C maestro |
| Extractor (gate MOSFET) | GPIO10 | Salida PWM (LEDC) / ON-OFF |
| LED alarma | GPIO11 | — |
| Buzzer | GPIO12 | Activo por nivel |
| UART debug | GPIO16 / GPIO17 | TX / RX consola |

---

## Estrategia de control

| Modo | Descripción |
|------|-------------|
| ON/OFF | Umbral fijo de gas → extractor encendido hasta retornar bajo umbral. Mínimo viable. |
| PID por PWM | Control proporcional-integral-derivativo modulando la velocidad del extractor vía LEDC, según concentración de gas o temperatura. Mejor regulación y menor ruido acústico. |

**Conceptos clave:** lectura de sensores con temporización no bloqueante (`esp_timer`), ADC oneshot con `esp_adc`, generación PWM con `LEDC`, máquina de estados de alarma, conmutación de carga inductiva con protección flyback.

---

## Stack tecnológico

| Herramienta | Uso |
|-------------|-----|
| ESP-IDF v5.x | Framework principal |
| FreeRTOS | Multitarea y temporización |
| esp_adc | Lectura analógica del MQ-135 |
| LEDC | PWM del extractor |
| esp-idf-ssd1306 | Driver OLED |
| esp-mqtt (opcional) | Telemetría IoT |
| VS Code + ESP-IDF Extension | Entorno de desarrollo |

---

## Compilar y flashear

```bash
# Configurar target
idf.py set-target esp32c6

# Configurar parámetros (umbrales, WiFi/MQTT si aplica)
idf.py menuconfig

# Compilar, flashear y monitorear
idf.py -p COM<X> flash monitor
```

Para salir del monitor: `Ctrl + ]`

---

## Autor

**Yeison Dénnir Termal Cuastumal**  
Ingeniería Electrónica — Universidad Nacional de Colombia · 2026  
[GitHub](https://github.com/DENCODE31)
