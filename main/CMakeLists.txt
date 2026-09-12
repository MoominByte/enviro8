idf_component_register(
    SRCS "app_main.c" "app_config.c" "sensor_manager.c" "network.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_timer nvs_flash esp_driver_i2c esp_driver_gpio json
    EMBED_FILES "www/index.html" "www/openapi.json" "www/swagger.html"
)
