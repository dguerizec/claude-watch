# Auto-generate portal_html.h from portal.html at configure time
execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/gen_html_header.py"
            "${CMAKE_CURRENT_LIST_DIR}/portal.html"
            "${CMAKE_CURRENT_LIST_DIR}/portal_html.h"
)

idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash dns_server log
)

# Force CMake reconfigure when portal.html changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/portal.html"
)
