FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES gc9a01 chsc6x display_text qr_display wifi_manager api_client usage_store polar_graph
)
