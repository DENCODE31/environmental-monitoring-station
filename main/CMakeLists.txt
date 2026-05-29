idf_component_register(
    SRCS "provisioning.c" "app_config.c" "status_led.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_hw_support
        log
        driver
        esp_adc
        esp_driver_ledc
        mqtt
        freertos
        json
        esp_http_server
        mdns
        lwip
        led_strip
    EMBED_TXTFILES
        "certs/aws_root_ca.pem"
        "certs/aws_device_cert.pem"
        "certs/aws_device_key.pem"
)
