# 学习导读：apps/stm32/stm32h743i_eval_renode/CMakeLists.txt
# 所属层次：STM32 应用层。
# 阅读重点：这里把 FreeRTOS Cortex-M7 port、c-claw runtime 和 Renode 启动文件链接成 STM32H743 ELF。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
cmake_minimum_required(VERSION 3.16)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(c_claw_stm32h743i_eval_renode C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(STM32H743_RENODE_CPU_FLAGS
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
)
set(STM32H743_RENODE_COMPILE_FLAGS
    ${STM32H743_RENODE_CPU_FLAGS}
    -ffunction-sections
    -fdata-sections
)
add_compile_options(${STM32H743_RENODE_COMPILE_FLAGS})

if(NOT DEFINED FREERTOS_KERNEL_PATH)
    set(FREERTOS_KERNEL_PATH "$ENV{HOME}/opt/FreeRTOS-Kernel" CACHE PATH "FreeRTOS-Kernel checkout")
endif()
if(NOT DEFINED STM32CUBEH7_PATH)
    set(STM32CUBEH7_PATH "$ENV{HOME}/opt/STM32CubeH7" CACHE PATH "STM32CubeH7 checkout")
endif()

set(FREERTOS_PORT_DIR "${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM7/r0p1")
set(FREERTOS_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/src")
set(STM32H7_HAL_DIR "${STM32CUBEH7_PATH}/Drivers/STM32H7xx_HAL_Driver")
set(STM32H7_CMSIS_DEVICE_DIR "${STM32CUBEH7_PATH}/Drivers/CMSIS/Device/ST/STM32H7xx")
set(STM32H7_CMSIS_CORE_DIR "${STM32CUBEH7_PATH}/Drivers/CMSIS/Core")
set(LWIP_DIR "${STM32CUBEH7_PATH}/Middlewares/Third_Party/LwIP")
set(FATFS_DIR "${STM32CUBEH7_PATH}/Middlewares/Third_Party/FatFs")
set(MBEDTLS_DIR "${STM32CUBEH7_PATH}/Middlewares/Third_Party/mbedTLS")
set(MBEDTLS_CONFIG_DIR "${STM32CUBEH7_PATH}/Projects/STM32H743I-EVAL/Applications/mbedTLS/SSL_Client/Inc")
set(LWIP_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${LWIP_DIR}/src/include"
    "${LWIP_DIR}/contrib/ports/freertos/include"
    CACHE INTERNAL "lwIP include directories for FreeRTOS platform")
set(CCLAW_STM32H743_STATIC_IP "$ENV{CCLAW_STM32H743_STATIC_IP}" CACHE STRING "STM32H743 Renode static IPv4 address")
set(CCLAW_STM32H743_HTTP_SMOKE_URL "$ENV{CCLAW_STM32H743_HTTP_SMOKE_URL}" CACHE STRING "STM32H743 Renode HTTP smoke URL")
set(CCLAW_STM32H743_ENABLE_HTTP_SMOKE "$ENV{CCLAW_STM32H743_ENABLE_HTTP_SMOKE}" CACHE STRING "Enable STM32H743 HTTP smoke at runtime")
set(CCLAW_STM32H743_ENABLE_REAL_LLM "$ENV{CCLAW_STM32H743_ENABLE_REAL_LLM}" CACHE STRING "Enable STM32H743 real LLM HTTPS smoke")
set(CCLAW_STM32H743_ENABLE_UART_CHAT "$ENV{CCLAW_STM32H743_ENABLE_UART_CHAT}" CACHE STRING "Enable STM32H743 UART real LLM chat")
set(CCLAW_STM32H743_REAL_LLM_ITERATIONS "$ENV{CCLAW_STM32H743_REAL_LLM_ITERATIONS}" CACHE STRING "STM32H743 real LLM smoke iteration count")
set(CCLAW_STM32H743_DNS_SERVER "$ENV{CCLAW_STM32H743_DNS_SERVER}" CACHE STRING "STM32H743 DNS server IPv4 address")
set(CCLAW_STM32H743_REAL_CONFIG_DIR "$ENV{CCLAW_STM32H743_REAL_CONFIG_DIR}" CACHE PATH "Directory containing generated real LLM config header")
set(CCLAW_STM32H743_ENABLE_FATFS "$ENV{CCLAW_STM32H743_ENABLE_FATFS}" CACHE STRING "Enable STM32H743 FatFs workspace")
set(CCLAW_STM32H743_SD_SIZE_MB "$ENV{CCLAW_STM32H743_SD_SIZE_MB}" CACHE STRING "STM32H743 Renode SD image size in MiB")
if(CCLAW_STM32H743_STATIC_IP STREQUAL "")
    set(CCLAW_STM32H743_STATIC_IP "10.0.2.15")
endif()
if(CCLAW_STM32H743_HTTP_SMOKE_URL STREQUAL "")
    set(CCLAW_STM32H743_HTTP_SMOKE_URL "http://10.0.2.2:8080/smoke")
endif()
if(CCLAW_STM32H743_ENABLE_HTTP_SMOKE STREQUAL "")
    set(CCLAW_STM32H743_ENABLE_HTTP_SMOKE "0")
endif()
if(CCLAW_STM32H743_ENABLE_REAL_LLM STREQUAL "")
    set(CCLAW_STM32H743_ENABLE_REAL_LLM "0")
endif()
if(CCLAW_STM32H743_ENABLE_UART_CHAT STREQUAL "")
    set(CCLAW_STM32H743_ENABLE_UART_CHAT "0")
endif()
if(CCLAW_STM32H743_REAL_LLM_ITERATIONS STREQUAL "")
    set(CCLAW_STM32H743_REAL_LLM_ITERATIONS "1")
endif()
if(CCLAW_STM32H743_DNS_SERVER STREQUAL "")
    set(CCLAW_STM32H743_DNS_SERVER "10.0.2.2")
endif()
if(CCLAW_STM32H743_ENABLE_FATFS STREQUAL "")
    set(CCLAW_STM32H743_ENABLE_FATFS "1")
endif()
if(CCLAW_STM32H743_SD_SIZE_MB STREQUAL "")
    set(CCLAW_STM32H743_SD_SIZE_MB "64")
endif()
math(EXPR CCLAW_STM32H743_SD_SECTOR_COUNT "${CCLAW_STM32H743_SD_SIZE_MB} * 1024 * 1024 / 512")
if(NOT EXISTS "${FREERTOS_KERNEL_PATH}/include/FreeRTOS.h")
    message(FATAL_ERROR "Set FREERTOS_KERNEL_PATH to a FreeRTOS-Kernel checkout")
endif()
if(NOT EXISTS "${FREERTOS_PORT_DIR}/port.c")
    message(FATAL_ERROR "FreeRTOS GCC/ARM_CM7/r0p1 port not found under ${FREERTOS_PORT_DIR}")
endif()
if(NOT EXISTS "${STM32H7_HAL_DIR}/Inc/stm32h7xx_hal.h")
    message(FATAL_ERROR "Set STM32CUBEH7_PATH to a STM32CubeH7 checkout with HAL submodules")
endif()
if(NOT EXISTS "${LWIP_DIR}/src/include/lwip/init.h")
    message(FATAL_ERROR "STM32CubeH7 lwIP middleware not found under ${LWIP_DIR}")
endif()
if(CCLAW_STM32H743_ENABLE_FATFS AND NOT EXISTS "${FATFS_DIR}/src/ff.h")
    message(FATAL_ERROR "STM32CubeH7 FatFs middleware not found under ${FATFS_DIR}; run scripts/stm32h743_renode.sh install-cubeh7")
endif()
if(CCLAW_STM32H743_ENABLE_REAL_LLM OR CCLAW_STM32H743_ENABLE_UART_CHAT)
    if(NOT EXISTS "${MBEDTLS_DIR}/include/mbedtls/ssl.h")
        message(FATAL_ERROR "STM32CubeH7 mbedTLS middleware not found under ${MBEDTLS_DIR}; run scripts/stm32h743_renode.sh install-cubeh7")
    endif()
    if(NOT EXISTS "${MBEDTLS_CONFIG_DIR}/mbedtls_config.h")
        message(FATAL_ERROR "STM32CubeH7 mbedTLS SSL client config not found under ${MBEDTLS_CONFIG_DIR}")
    endif()
    if(CCLAW_STM32H743_REAL_CONFIG_DIR STREQUAL "" OR NOT EXISTS "${CCLAW_STM32H743_REAL_CONFIG_DIR}/cclaw_stm32h743_real_config.h")
        message(FATAL_ERROR "Generated real LLM config header not found; use scripts/stm32h743_renode.sh real-tls or stress")
    endif()
    set(CCLAW_FREERTOS_ENABLE_MBEDTLS ON CACHE BOOL "Enable mbedTLS in the FreeRTOS platform HTTP client" FORCE)
    set(MBEDTLS_INCLUDE_DIRS
        "${MBEDTLS_DIR}/include"
        "${MBEDTLS_CONFIG_DIR}"
        CACHE INTERNAL "mbedTLS include directories for FreeRTOS platform")
endif()

set(CC_PROFILE stm32h743-renode CACHE STRING "c-claw build profile" FORCE)
set(CC_TARGET_PLATFORM freertos CACHE STRING "c-claw target platform" FORCE)
set(CC_ENABLE_FILE_TOOLS ON CACHE BOOL "Enable SD-card backed file tools" FORCE)
set(CC_ENABLE_HTTP_TOOL ON CACHE BOOL "Enable http.request for STM32 lwIP smoke" FORCE)
set(CC_ENABLE_MEMORY OFF CACHE BOOL "Disable memory tools for bare-metal Renode smoke" FORCE)
set(CC_ENABLE_RUN_QUEUE OFF CACHE BOOL "Disable run queue for first STM32H743 Renode smoke" FORCE)

if(CCLAW_STM32H743_ENABLE_FATFS)
    set(CCLAW_FREERTOS_ENABLE_FATFS ON CACHE BOOL "Enable FatFs in the FreeRTOS platform filesystem" FORCE)
    set(FATFS_INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${FATFS_DIR}/src"
        CACHE INTERNAL "FatFs include directories for FreeRTOS platform")
endif()

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../../cclaw"
    "${CMAKE_BINARY_DIR}/c_claw"
)

set(FREERTOS_SOURCES
    "${FREERTOS_KERNEL_PATH}/croutine.c"
    "${FREERTOS_KERNEL_PATH}/event_groups.c"
    "${FREERTOS_KERNEL_PATH}/list.c"
    "${FREERTOS_KERNEL_PATH}/queue.c"
    "${FREERTOS_KERNEL_PATH}/stream_buffer.c"
    "${FREERTOS_KERNEL_PATH}/tasks.c"
    "${FREERTOS_KERNEL_PATH}/timers.c"
    "${FREERTOS_KERNEL_PATH}/portable/MemMang/heap_4.c"
    "${FREERTOS_PORT_DIR}/port.c"
)

set(STM32H7_HAL_SOURCES
    "${STM32H7_CMSIS_DEVICE_DIR}/Source/Templates/system_stm32h7xx.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_cortex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_dma.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_eth.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_eth_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_flash.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_flash_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_gpio.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_pwr.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_pwr_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_rcc.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_rcc_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_sd.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_sd_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_uart.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_hal_uart_ex.c"
    "${STM32H7_HAL_DIR}/Src/stm32h7xx_ll_sdmmc.c"
)

set(LWIP_SOURCES
    "${LWIP_DIR}/src/api/api_lib.c"
    "${LWIP_DIR}/src/api/api_msg.c"
    "${LWIP_DIR}/src/api/err.c"
    "${LWIP_DIR}/src/api/netbuf.c"
    "${LWIP_DIR}/src/api/netdb.c"
    "${LWIP_DIR}/src/api/netifapi.c"
    "${LWIP_DIR}/src/api/sockets.c"
    "${LWIP_DIR}/src/api/tcpip.c"
    "${LWIP_DIR}/src/core/def.c"
    "${LWIP_DIR}/src/core/dns.c"
    "${LWIP_DIR}/src/core/inet_chksum.c"
    "${LWIP_DIR}/src/core/init.c"
    "${LWIP_DIR}/src/core/ip.c"
    "${LWIP_DIR}/src/core/mem.c"
    "${LWIP_DIR}/src/core/memp.c"
    "${LWIP_DIR}/src/core/netif.c"
    "${LWIP_DIR}/src/core/pbuf.c"
    "${LWIP_DIR}/src/core/raw.c"
    "${LWIP_DIR}/src/core/stats.c"
    "${LWIP_DIR}/src/core/sys.c"
    "${LWIP_DIR}/src/core/tcp.c"
    "${LWIP_DIR}/src/core/tcp_in.c"
    "${LWIP_DIR}/src/core/tcp_out.c"
    "${LWIP_DIR}/src/core/timeouts.c"
    "${LWIP_DIR}/src/core/udp.c"
    "${LWIP_DIR}/src/core/ipv4/autoip.c"
    "${LWIP_DIR}/src/core/ipv4/etharp.c"
    "${LWIP_DIR}/src/core/ipv4/icmp.c"
    "${LWIP_DIR}/src/core/ipv4/igmp.c"
    "${LWIP_DIR}/src/core/ipv4/ip4.c"
    "${LWIP_DIR}/src/core/ipv4/ip4_addr.c"
    "${LWIP_DIR}/src/core/ipv4/ip4_frag.c"
    "${LWIP_DIR}/src/netif/ethernet.c"
    "${LWIP_DIR}/contrib/ports/freertos/sys_arch.c"
)

set(MBEDTLS_SOURCES)
if(CCLAW_STM32H743_ENABLE_REAL_LLM OR CCLAW_STM32H743_ENABLE_UART_CHAT)
    file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS "${MBEDTLS_DIR}/library/*.c")
endif()

set(FATFS_SOURCES)
if(CCLAW_STM32H743_ENABLE_FATFS)
    set(FATFS_SOURCES
        "${FATFS_DIR}/src/ff.c"
        "${FATFS_DIR}/src/diskio.c"
        "${FATFS_DIR}/src/ff_gen_drv.c"
        "${FATFS_DIR}/src/option/unicode.c"
    )
endif()

add_executable(c_claw_stm32h743i_eval_renode
    src/board.c
    src/board_storage.c
    src/ethernetif.c
    src/http_smoke.c
    src/main.c
    src/network.c
    src/real_llm_smoke.c
    src/startup.c
    src/syscalls.c
    src/stm32_sd_diskio.c
    src/uart_chat.c
    ${FREERTOS_SOURCES}
    ${STM32H7_HAL_SOURCES}
    ${LWIP_SOURCES}
    ${FATFS_SOURCES}
    ${MBEDTLS_SOURCES}
)

target_include_directories(c_claw_stm32h743i_eval_renode PRIVATE
    src
    "${STM32H7_HAL_DIR}/Inc"
    "${STM32H7_CMSIS_DEVICE_DIR}/Include"
    "${STM32H7_CMSIS_CORE_DIR}/Include"
    ${LWIP_INCLUDE_DIRS}
    ${FATFS_INCLUDE_DIRS}
    "${FREERTOS_KERNEL_PATH}/include"
    "${FREERTOS_PORT_DIR}"
)

if(CCLAW_STM32H743_ENABLE_REAL_LLM OR CCLAW_STM32H743_ENABLE_UART_CHAT)
    target_include_directories(c_claw_stm32h743i_eval_renode PRIVATE
        "${MBEDTLS_DIR}/include"
        "${MBEDTLS_CONFIG_DIR}"
        "${CCLAW_STM32H743_REAL_CONFIG_DIR}"
    )
endif()

target_compile_definitions(c_claw_stm32h743i_eval_renode PRIVATE
    STM32H743xx
    USE_HAL_DRIVER
    CCLAW_STM32H743_STATIC_IP="${CCLAW_STM32H743_STATIC_IP}"
    CCLAW_STM32H743_HTTP_SMOKE_URL="${CCLAW_STM32H743_HTTP_SMOKE_URL}"
    CCLAW_STM32H743_ENABLE_HTTP_SMOKE=${CCLAW_STM32H743_ENABLE_HTTP_SMOKE}
    CCLAW_STM32H743_ENABLE_REAL_LLM=${CCLAW_STM32H743_ENABLE_REAL_LLM}
    CCLAW_STM32H743_ENABLE_UART_CHAT=${CCLAW_STM32H743_ENABLE_UART_CHAT}
    CCLAW_STM32H743_ENABLE_FATFS=${CCLAW_STM32H743_ENABLE_FATFS}
    CCLAW_STM32H743_SD_SECTOR_COUNT=${CCLAW_STM32H743_SD_SECTOR_COUNT}
    CCLAW_STM32H743_REAL_LLM_ITERATIONS=${CCLAW_STM32H743_REAL_LLM_ITERATIONS}
    CCLAW_STM32H743_DNS_SERVER="${CCLAW_STM32H743_DNS_SERVER}"
)

if(CCLAW_STM32H743_ENABLE_REAL_LLM OR CCLAW_STM32H743_ENABLE_UART_CHAT)
    target_compile_definitions(c_claw_stm32h743i_eval_renode PRIVATE
        CCLAW_FREERTOS_ENABLE_MBEDTLS=1
        MBEDTLS_CONFIG_FILE=<mbedtls_config.h>
    )
endif()

target_compile_options(c_claw_stm32h743i_eval_renode PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_options(c_claw_stm32h743i_eval_renode PRIVATE
    -T "${CMAKE_CURRENT_LIST_DIR}/stm32h743i_eval_renode.ld"
    ${STM32H743_RENODE_CPU_FLAGS}
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/c_claw_stm32h743i_eval_renode.map
)

target_link_libraries(c_claw_stm32h743i_eval_renode PRIVATE
    c_claw_runtime
)
