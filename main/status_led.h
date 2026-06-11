/* ============================================================
 *   status_led — LED RGB de estado (WS2812 onboard, GPIO8)
 * ============================================================
 *   Indica visualmente el estado de red y del botón RESET:
 *
 *     OFF           : apagado
 *     PORTAL        : rojo parpadeando lento (1 Hz)    → portal cautivo
 *     RECONNECTING  : naranja parpadeando rápido (3 Hz) → buscando WiFi
 *     CONNECTED     : azul fijo                         → con IP
 *     RESET_HOLD    : magenta pulsando rápido (6 Hz)    → botón RESET presionado
 *
 *   `push/pop` permiten entrar a RESET_HOLD mientras se sostiene
 *   el botón y volver al estado anterior si se libera antes de 3 s.
 * ============================================================ */
#pragma once

typedef enum {
    LED_ST_OFF = 0,
    LED_ST_PORTAL,        /* Rojo lento — portal cautivo activo            */
    LED_ST_RECONNECTING,  /* Naranja rápido — sin enlace, reintentando WiFi */
    LED_ST_CONNECTED,     /* Azul fijo — conectado con IP                  */
    LED_ST_RESET_HOLD,    /* Magenta pulsando — botón RESET sostenido      */
} led_state_t;

/* Inicializa el LED WS2812 y lanza la tarea de parpadeo */
void status_led_init(void);

/* Cambia el estado mostrado por el LED (seguro entre tareas) */
void status_led_set(led_state_t st);

/* Guarda el estado actual y entra a 'st'. Útil para mostrar feedback
 * temporal (p.ej. RESET_HOLD) sin perder el estado de red anterior. */
void status_led_push_state(led_state_t st);

/* Restaura el último estado guardado por push_state. No-op si la pila está vacía. */
void status_led_pop_state(void);
