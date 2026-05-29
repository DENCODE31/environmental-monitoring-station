/* ============================================================
 *   status_led — LED RGB de estado (WS2812 onboard, GPIO8)
 * ============================================================
 *   Indica visualmente el estado de red del dispositivo:
 *     - CONFIG    : rojo parpadeando (portal cautivo o reconectando)
 *     - CONNECTED : azul fijo (conectado a la red con IP)
 *     - OFF       : apagado
 *   Una tarea dedicada refresca el LED según el estado actual.
 * ============================================================ */
#pragma once

typedef enum {
    LED_ST_OFF = 0,       /* Apagado                          */
    LED_ST_CONFIG,        /* Rojo parpadeando: configurando   */
    LED_ST_CONNECTED,     /* Azul fijo: conectado             */
} led_state_t;

/* Inicializa el LED WS2812 y lanza la tarea de parpadeo */
void status_led_init(void);

/* Cambia el estado mostrado por el LED (seguro entre tareas) */
void status_led_set(led_state_t st);
