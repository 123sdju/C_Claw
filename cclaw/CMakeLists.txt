# 学习导读：cclaw/CMakeLists.txt
# 所属层次：构建脚本。
# 阅读重点：这里描述目标、源码、依赖和编译宏，阅读时重点看 profile 如何决定最终被编译的模块。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(c_claw C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers")

set(CC_PROFILE "posix-cli" CACHE STRING "Build profile: posix-cli, core-minimal, stm32mp135-board")
set_property(CACHE CC_PROFILE PROPERTY STRINGS posix-cli core-minimal stm32mp135-board)

if(NOT DEFINED CC_PROFILE_APPLICATION_DIR)
    set(CC_PROFILE_APPLICATION_DIR "")
endif()

set(CC_PROFILE_FILE "${CMAKE_CURRENT_LIST_DIR}/profiles/${CC_PROFILE}.cmake")
if(NOT EXISTS "${CC_PROFILE_FILE}")
    message(FATAL_ERROR "Unknown CC_PROFILE: ${CC_PROFILE}")
endif()
# profile 文件只设置默认值；用户仍可在 cmake 命令行里覆盖 CACHE 选项。
include("${CC_PROFILE_FILE}")

# CC_TARGET_PLATFORM 可以显式指定，也可以 auto；resolved 值会驱动 platforms/<name> 子目录。
set(CC_TARGET_PLATFORM "${CC_PROFILE_TARGET_PLATFORM}" CACHE STRING "Target platform: auto, posix, windows, esp32, freertos")
set_property(CACHE CC_TARGET_PLATFORM PROPERTY STRINGS auto posix windows esp32 freertos)

if(CC_TARGET_PLATFORM STREQUAL "auto")
    if(DEFINED ESP_PLATFORM)
        set(CC_TARGET_PLATFORM_RESOLVED "esp32")
    elseif(WIN32)
        set(CC_TARGET_PLATFORM_RESOLVED "windows")
    else()
        set(CC_TARGET_PLATFORM_RESOLVED "posix")
    endif()
else()
    set(CC_TARGET_PLATFORM_RESOLVED "${CC_TARGET_PLATFORM}")
endif()
set(CC_TARGET_PLATFORM_RESOLVED "${CC_TARGET_PLATFORM_RESOLVED}" CACHE INTERNAL "Resolved c-claw platform")

# 平台 ID 会变成 C 宏 CC_PLATFORM，供 cc_platform.h 推导平台能力。
if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "posix")
    set(CC_PLATFORM_ID 100)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows")
    set(CC_PLATFORM_ID 200)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32")
    set(CC_PLATFORM_ID 300)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    set(CC_PLATFORM_ID 400)
else()
    message(FATAL_ERROR "Unknown CC_TARGET_PLATFORM: ${CC_TARGET_PLATFORM}")
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT ESP_PLATFORM)
    message(FATAL_ERROR
        "CC_TARGET_PLATFORM=esp32 must be configured from ESP-IDF "
        "(ESP_PLATFORM is not set).")
endif()

# 同一个 build 目录不能在 posix/windows/esp32 间反复切换，否则对象文件和配置缓存会混在一起。
if(DEFINED CC_CACHED_TARGET_PLATFORM AND
   NOT CC_CACHED_TARGET_PLATFORM STREQUAL CC_TARGET_PLATFORM_RESOLVED)
    message(FATAL_ERROR
        "This build directory was configured for '${CC_CACHED_TARGET_PLATFORM}' "
        "and is now being reconfigured for '${CC_TARGET_PLATFORM_RESOLVED}'. "
        "Use a fresh build directory.")
endif()
set(CC_CACHED_TARGET_PLATFORM "${CC_TARGET_PLATFORM_RESOLVED}"
    CACHE INTERNAL "Resolved c-claw platform profile for this build directory")

option(CC_ENABLE_FILE_TOOLS "Enable file read/write tools" ${CC_PROFILE_ENABLE_FILE_TOOLS})
option(CC_ENABLE_HTTP_TOOL "Enable http.request tool" ${CC_PROFILE_ENABLE_HTTP_TOOL})
option(CC_ENABLE_MEMORY "Enable long-term memory subsystem" ${CC_PROFILE_ENABLE_MEMORY})
option(CC_ENABLE_CLI "Enable CLI gateway executable" ${CC_PROFILE_ENABLE_CLI})
option(CC_ENABLE_SHELL "Enable shell.run tool and local sandbox" ${CC_PROFILE_ENABLE_SHELL})
option(CC_ENABLE_PLUGIN "Enable external process plugin tools" ${CC_PROFILE_ENABLE_PLUGIN})
option(CC_ENABLE_MULTI_AGENT "Enable multi-agent manager core" ${CC_PROFILE_ENABLE_MULTI_AGENT})
option(CC_ENABLE_RUN_QUEUE "Enable run queue and session serialization" ${CC_PROFILE_ENABLE_RUN_QUEUE})
option(CC_ENABLE_TOOL_POOL "Enable tool executor concurrency pool" ${CC_PROFILE_ENABLE_TOOL_POOL})
option(CC_ENABLE_SKILLS "Enable AgentSkills-compatible skill catalog" ${CC_PROFILE_ENABLE_SKILLS})
option(CC_ENABLE_SKILL_WATCHER "Enable app-level skill file watcher support" ${CC_PROFILE_ENABLE_SKILL_WATCHER})
option(CC_ENABLE_PLUGIN_HOT_RELOAD "Enable plugin hot reload generation swapping" ${CC_PROFILE_ENABLE_PLUGIN_HOT_RELOAD})
option(CC_ENABLE_PLUGIN_WORKERS "Enable plugin worker pools" ${CC_PROFILE_ENABLE_PLUGIN_WORKERS})
option(CC_ENABLE_MCP "Enable MCP runtime abstractions and tool bridge" ${CC_PROFILE_ENABLE_MCP})
option(CC_ENABLE_MCP_STDIO "Enable stdio MCP transport in desktop apps" ${CC_PROFILE_ENABLE_MCP_STDIO})
option(CC_ENABLE_MCP_HTTP "Enable HTTP/SSE MCP transport" ${CC_PROFILE_ENABLE_MCP_HTTP})
option(CC_ENABLE_SUBAGENTS "Enable subagent services" ${CC_PROFILE_ENABLE_SUBAGENTS})
option(CC_ENABLE_ACTIVE_MEMORY "Enable active memory hook for run context and summary writes" ${CC_PROFILE_ENABLE_ACTIVE_MEMORY})
option(CC_ENABLE_DOCKER_SANDBOX "Enable Docker sandbox adapter" ${CC_PROFILE_ENABLE_DOCKER_SANDBOX})
option(CC_ENABLE_SQLITE "Enable SQLite storage backend" ${CC_PROFILE_ENABLE_SQLITE})
option(CC_ENABLE_OPENAI "Enable OpenAI-compatible LLM adapter" ${CC_PROFILE_ENABLE_OPENAI})
option(CC_ENABLE_OLLAMA "Enable Ollama LLM adapter" ${CC_PROFILE_ENABLE_OLLAMA})
option(CC_ENABLE_ANTHROPIC "Enable Anthropic LLM adapter" ${CC_PROFILE_ENABLE_ANTHROPIC})
if(NOT DEFINED CC_PROFILE_ENABLE_BOARD_TOOLS)
    set(CC_PROFILE_ENABLE_BOARD_TOOLS OFF)
endif()
option(CC_ENABLE_BOARD_TOOLS "Enable STM32MP135 board tools" ${CC_PROFILE_ENABLE_BOARD_TOOLS})

message(STATUS "c-claw profile: ${CC_PROFILE}")
message(STATUS "c-claw target platform: ${CC_TARGET_PLATFORM_RESOLVED}")

# 桌面平台需要线程库；ESP32 线程能力由 ESP-IDF 适配层提供。
set(THREADS_PREFER_PTHREAD_FLAG ON)
if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    find_package(Threads REQUIRED)
endif()

# 只要启用 HTTP 工具或任一 HTTP-backed LLM，就需要编译平台 HTTP client。
set(CC_NEEDS_HTTP_CLIENT OFF)
if(CC_ENABLE_HTTP_TOOL OR CC_ENABLE_OPENAI OR CC_ENABLE_OLLAMA OR CC_ENABLE_ANTHROPIC OR CC_ENABLE_MCP_HTTP)
    set(CC_NEEDS_HTTP_CLIENT ON)
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows" AND CC_ENABLE_DOCKER_SANDBOX)
    message(STATUS "Docker sandbox is disabled for Windows targets")
    set(CC_ENABLE_DOCKER_SANDBOX OFF CACHE BOOL "Enable Docker sandbox adapter" FORCE)
endif()

# 这些默认路径被编译成宏，配置文件缺失时 cc_config_load_default 会使用它们。
if(NOT DEFINED CC_PROFILE_DEFAULT_CONFIG_PATH)
    set(CC_PROFILE_DEFAULT_CONFIG_PATH "")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_DATA_DIR)
    set(CC_PROFILE_DEFAULT_DATA_DIR "${CMAKE_BINARY_DIR}/${CC_PROFILE}/runtime/data")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_WORKSPACE_PATH)
    set(CC_PROFILE_DEFAULT_WORKSPACE_PATH "${CMAKE_BINARY_DIR}/${CC_PROFILE}/runtime/workspace")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_STORAGE_PATH)
    if(CC_ENABLE_SQLITE)
        set(CC_PROFILE_DEFAULT_STORAGE_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/c-claw.db")
    else()
        set(CC_PROFILE_DEFAULT_STORAGE_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/sessions.json")
    endif()
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_MEMORY_PATH)
    set(CC_PROFILE_DEFAULT_MEMORY_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/memory.json")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_SOUL_FILE)
    set(CC_PROFILE_DEFAULT_SOUL_FILE "")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_USER_FILE)
    set(CC_PROFILE_DEFAULT_USER_FILE "")
endif()

set(CC_DEFAULT_CONFIG_PATH_VALUE "${CC_PROFILE_DEFAULT_CONFIG_PATH}")
set(CC_DEFAULT_DATA_DIR_VALUE "${CC_PROFILE_DEFAULT_DATA_DIR}")
set(CC_DEFAULT_WORKSPACE_PATH_VALUE "${CC_PROFILE_DEFAULT_WORKSPACE_PATH}")
set(CC_DEFAULT_STORAGE_PATH_VALUE "${CC_PROFILE_DEFAULT_STORAGE_PATH}")
set(CC_DEFAULT_MEMORY_PATH_VALUE "${CC_PROFILE_DEFAULT_MEMORY_PATH}")
set(CC_DEFAULT_SOUL_FILE_VALUE "${CC_PROFILE_DEFAULT_SOUL_FILE}")
set(CC_DEFAULT_USER_FILE_VALUE "${CC_PROFILE_DEFAULT_USER_FILE}")

# 桌面构建提前创建 runtime/data 和 workspace，避免首次运行时工具写入失败。
if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    file(MAKE_DIRECTORY
        "${CC_DEFAULT_DATA_DIR_VALUE}"
        "${CC_DEFAULT_WORKSPACE_PATH_VALUE}"
    )
endif()

# c_claw_project_options 是全仓库共享的编译契约：功能开关、平台能力和默认路径都从这里传入 C 代码。
add_library(c_claw_project_options INTERFACE)
target_compile_definitions(c_claw_project_options INTERFACE
    CC_PLATFORM=${CC_PLATFORM_ID}
    CC_TOOL_SHELL_RUN=$<BOOL:${CC_ENABLE_SHELL}>
    CC_SANDBOX_LOCAL=$<BOOL:${CC_ENABLE_SHELL}>
    CC_SANDBOX_DOCKER=$<BOOL:${CC_ENABLE_DOCKER_SANDBOX}>
    CC_TOOL_HTTP_REQUEST=$<BOOL:${CC_ENABLE_HTTP_TOOL}>
    CC_TOOL_FILE_READ=$<BOOL:${CC_ENABLE_FILE_TOOLS}>
    CC_TOOL_FILE_WRITE=$<BOOL:${CC_ENABLE_FILE_TOOLS}>
    CC_TOOL_PLUGIN=$<BOOL:${CC_ENABLE_PLUGIN}>
    CC_ENABLE_MULTI_AGENT=$<BOOL:${CC_ENABLE_MULTI_AGENT}>
    CC_ENABLE_RUN_QUEUE=$<BOOL:${CC_ENABLE_RUN_QUEUE}>
    CC_ENABLE_TOOL_POOL=$<BOOL:${CC_ENABLE_TOOL_POOL}>
    CC_ENABLE_SKILLS=$<BOOL:${CC_ENABLE_SKILLS}>
    CC_ENABLE_SKILL_WATCHER=$<BOOL:${CC_ENABLE_SKILL_WATCHER}>
    CC_ENABLE_PLUGIN_HOT_RELOAD=$<BOOL:${CC_ENABLE_PLUGIN_HOT_RELOAD}>
    CC_ENABLE_PLUGIN_WORKERS=$<BOOL:${CC_ENABLE_PLUGIN_WORKERS}>
    CC_ENABLE_MCP=$<BOOL:${CC_ENABLE_MCP}>
    CC_ENABLE_MCP_STDIO=$<BOOL:${CC_ENABLE_MCP_STDIO}>
    CC_ENABLE_MCP_HTTP=$<BOOL:${CC_ENABLE_MCP_HTTP}>
    CC_ENABLE_SUBAGENTS=$<BOOL:${CC_ENABLE_SUBAGENTS}>
    CC_ENABLE_ACTIVE_MEMORY=$<BOOL:${CC_ENABLE_ACTIVE_MEMORY}>
    CC_STORAGE_SQLITE=$<BOOL:${CC_ENABLE_SQLITE}>
    CC_STORAGE_JSON_FILE=1
    CC_STORAGE_MEMORY=1
    CC_LLM_OPENAI=$<BOOL:${CC_ENABLE_OPENAI}>
    CC_LLM_OLLAMA=$<BOOL:${CC_ENABLE_OLLAMA}>
    CC_LLM_ANTHROPIC=$<BOOL:${CC_ENABLE_ANTHROPIC}>
    CC_ENABLE_BOARD_TOOLS=$<BOOL:${CC_ENABLE_BOARD_TOOLS}>
    CC_HAS_MEMORY=$<BOOL:${CC_ENABLE_MEMORY}>
    CC_GATEWAY_CLI=$<BOOL:${CC_ENABLE_CLI}>
    CC_DEFAULT_CONFIG_PATH="${CC_DEFAULT_CONFIG_PATH_VALUE}"
    CC_DEFAULT_DATA_DIR="${CC_DEFAULT_DATA_DIR_VALUE}"
    CC_DEFAULT_WORKSPACE_PATH="${CC_DEFAULT_WORKSPACE_PATH_VALUE}"
    CC_DEFAULT_STORAGE_PATH="${CC_DEFAULT_STORAGE_PATH_VALUE}"
    CC_DEFAULT_MEMORY_PATH="${CC_DEFAULT_MEMORY_PATH_VALUE}"
    CC_DEFAULT_SOUL_FILE="${CC_DEFAULT_SOUL_FILE_VALUE}"
    CC_DEFAULT_USER_FILE="${CC_DEFAULT_USER_FILE_VALUE}"
)

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows" AND NOT MINGW)
    target_compile_definitions(c_claw_project_options INTERFACE
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0600)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    target_compile_definitions(c_claw_project_options INTERFACE
        CC_FREERTOS_PLATFORM=1
        CC_HAS_NETWORK=$<BOOL:${CC_NEEDS_HTTP_CLIENT}>)
elseif(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32")
    target_compile_definitions(c_claw_project_options INTERFACE
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700)
endif()

# 先编译抽象端口和核心，再编译可选适配器，最后选中一个平台实现。
add_subdirectory(ports)
add_subdirectory(core)
add_subdirectory(adapters)
add_subdirectory("platforms/${CC_TARGET_PLATFORM_RESOLVED}")

# runtime 静态库把 core/adapters/platform 的对象文件合并，供 app 或测试链接。
set(CC_PLATFORM_OBJECT_TARGET "c_claw_platform_${CC_TARGET_PLATFORM_RESOLVED}_objects")
add_library(c_claw_runtime STATIC
    $<TARGET_OBJECTS:c_claw_core_objects>
    $<TARGET_OBJECTS:c_claw_adapters_objects>
    $<TARGET_OBJECTS:${CC_PLATFORM_OBJECT_TARGET}>
)
target_link_libraries(c_claw_runtime PUBLIC
    c_claw_core_headers
    c_claw_adapters_headers
    c_claw_ports_headers
    c_claw_project_options
)

if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    target_link_libraries(c_claw_runtime PUBLIC Threads::Threads)
    if(CMAKE_DL_LIBS)
        target_link_libraries(c_claw_runtime PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

if(CC_PLATFORM_PUBLIC_LIBRARIES)
    target_link_libraries(c_claw_runtime PUBLIC ${CC_PLATFORM_PUBLIC_LIBRARIES})
endif()
if(CC_PLATFORM_PUBLIC_INCLUDE_DIRS)
    target_include_directories(c_claw_runtime PUBLIC ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS})
endif()
