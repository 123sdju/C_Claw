# 学习导读：CMakeLists.txt
# 所属层次：构建脚本。
# 阅读重点：这里描述目标、源码、依赖和编译宏，阅读时重点看 profile 如何决定最终被编译的模块。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
cmake_minimum_required(VERSION 3.16)
project(c_claw_workspace C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# CC_PROFILE 是整个仓库的第一层入口开关：它决定默认 app 目录、
# 目标平台、功能裁剪以及运行时默认配置路径。
set(CC_PROFILE "posix-cli" CACHE STRING "Build profile: posix-cli, windows-cli, core-minimal, esp32-s3-qemu")
set_property(CACHE CC_PROFILE PROPERTY STRINGS posix-cli windows-cli core-minimal esp32-s3-qemu)

# 顶层 workspace 只选择应用目录；真正的平台和功能开关在 cclaw/profiles/*.cmake 中定义。
if(NOT DEFINED CC_PROFILE_APPLICATION_DIR)
    if(CC_PROFILE STREQUAL "posix-cli")
        set(CC_PROFILE_APPLICATION_DIR "apps/posix/cli")
    elseif(CC_PROFILE STREQUAL "windows-cli")
        set(CC_PROFILE_APPLICATION_DIR "apps/windows/cli")
    else()
        set(CC_PROFILE_APPLICATION_DIR "")
    endif()
endif()

# SDK 本体先被加入，profile 会在 cclaw/CMakeLists.txt 内解析并产出 c_claw_runtime。
add_subdirectory(cclaw)

# 有应用目录的 profile 会继续编译具体产品入口，例如 POSIX/Windows CLI。
if(CC_PROFILE_APPLICATION_DIR)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${CC_PROFILE_APPLICATION_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Profile application directory has no CMakeLists.txt: ${CC_PROFILE_APPLICATION_DIR}")
    endif()
    add_subdirectory("${CC_PROFILE_APPLICATION_DIR}")
elseif(CC_ENABLE_CLI)
    message(FATAL_ERROR "CC_ENABLE_CLI requires CC_PROFILE_APPLICATION_DIR")
endif()

# ESP32 走 ESP-IDF 测试/构建路径，桌面 profile 才启用 CTest。
if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32")
    enable_testing()
    add_subdirectory(cclaw/tests)
    if(CC_PROFILE_APPLICATION_DIR AND
       EXISTS "${CMAKE_CURRENT_LIST_DIR}/${CC_PROFILE_APPLICATION_DIR}/tests/CMakeLists.txt")
        add_subdirectory("${CC_PROFILE_APPLICATION_DIR}/tests")
    endif()
endif()
