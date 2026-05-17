# 学习导读：apps/esp32/esp32_s3_qemu/main/CMakeLists.txt
# 所属层次：ESP32 应用层。
# 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
idf_component_register(
    SRCS
        "cclaw_qemu_main.c"
        "cc_esp32_s3_qemu_features.c"
        "tools/cc_esp32_gpio_tool.c"
    INCLUDE_DIRS
        "../../../../cclaw/core/include"
        "../../../../cclaw/ports/include"
        "../../../../cclaw/adapters/include"
)

set(CC_PROFILE esp32-s3-qemu CACHE STRING "c-claw build profile" FORCE)
set(CC_TARGET_PLATFORM esp32 CACHE STRING "c-claw target platform" FORCE)
set(CC_ENABLE_CLI OFF CACHE BOOL "Disable CLI gateway for QEMU smoke test" FORCE)
set(CC_ENABLE_SHELL OFF CACHE BOOL "Disable shell tool for QEMU smoke test" FORCE)
set(CC_ENABLE_PLUGIN OFF CACHE BOOL "Disable plugin bridge for QEMU smoke test" FORCE)
set(CC_ENABLE_DOCKER_SANDBOX OFF CACHE BOOL "Disable Docker sandbox for QEMU smoke test" FORCE)
set(CC_ENABLE_SQLITE OFF CACHE BOOL "Disable SQLite for QEMU smoke test" FORCE)
set(CC_ENABLE_HTTP_TOOL OFF CACHE BOOL "Disable HTTP tool for QEMU smoke test" FORCE)
if("$ENV{CCLAW_QEMU_REAL_LLM}" STREQUAL "1")
    set(CC_ENABLE_OPENAI ON CACHE BOOL "Enable OpenAI-compatible provider for real QEMU chat" FORCE)
else()
    set(CC_ENABLE_OPENAI OFF CACHE BOOL "Disable OpenAI provider for QEMU smoke test" FORCE)
endif()
set(CC_ENABLE_OLLAMA OFF CACHE BOOL "Disable Ollama provider for QEMU smoke test" FORCE)
set(CC_ENABLE_ANTHROPIC OFF CACHE BOOL "Disable Anthropic provider for QEMU smoke test" FORCE)
set(CC_ENABLE_MEMORY ON CACHE BOOL "Keep lightweight memory code compiled" FORCE)
set(CC_ENABLE_FILE_TOOLS ON CACHE BOOL "Keep VFS-backed file tools compiled" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../../../cclaw"
    "${CMAKE_BINARY_DIR}/c_claw"
)

target_link_libraries(c_claw_runtime PUBLIC
    idf::freertos
    idf::newlib
    idf::vfs
    idf::esp_system
    idf::esp_event
    idf::esp_driver_gpio
    idf::esp_eth
    idf::esp_http_client
    idf::esp_netif
    idf::esp-tls
    idf::fatfs
    idf::mbedtls
    idf::nvs_flash
    idf::wear_levelling
)

target_link_libraries(${COMPONENT_LIB} PUBLIC
    c_claw_runtime
    idf::esp_driver_gpio
    idf::fatfs
    idf::wear_levelling
)

if("$ENV{CCLAW_QEMU_REAL_LLM}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CCLAW_QEMU_REAL_LLM=1
        CCLAW_QEMU_LLM_API_KEY="$ENV{CCLAW_QEMU_LLM_API_KEY}"
        CCLAW_QEMU_LLM_BASE_URL="$ENV{CCLAW_QEMU_LLM_BASE_URL}"
        CCLAW_QEMU_LLM_MODEL="$ENV{CCLAW_QEMU_LLM_MODEL}"
    )
endif()
