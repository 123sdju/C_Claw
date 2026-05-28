



add_library(c_claw_platform_esp32_objects OBJECT
    src/cc_esp32_filesystem.c
    src/cc_esp32_path.c
    src/cc_esp32_thread.c
)

if(CC_NEEDS_HTTP_CLIENT)
    target_sources(c_claw_platform_esp32_objects PRIVATE src/cc_esp32_http_client.c)
endif()

target_link_libraries(c_claw_platform_esp32_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/include"
    PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES "" PARENT_SCOPE)

if(ESP_PLATFORM)
    target_link_libraries(c_claw_platform_esp32_objects PRIVATE
        idf::freertos
        idf::newlib
        idf::vfs
        idf::esp_system
        idf::esp_event
        idf::esp_eth
        idf::esp_http_client
        idf::esp_netif
        idf::esp-tls
        idf::mbedtls
        idf::nvs_flash
    )
endif()
