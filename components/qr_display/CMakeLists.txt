idf_component_register(
    SRCS "qr_display.c" "qrcodegen.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_lcd
)
