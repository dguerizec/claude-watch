idf_component_register(
    SRCS "api_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json mbedtls wifi_manager
)
