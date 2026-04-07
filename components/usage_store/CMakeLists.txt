idf_component_register(
    SRCS "usage_store.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdspi
)
