idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_log
        driver
        esp_adc
        esp_driver_ledc
        mqtt
        freertos
)
