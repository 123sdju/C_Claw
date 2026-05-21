# 学习导读：apps/esp32/esp32_s3_qemu/CMakeLists.txt
# 所属层次：ESP32 应用层。
# 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 默认会把 sdkconfig 写在工程源码目录。这里显式改到 build 根，
# 让源码目录只保留 sdkconfig.defaults，所有机器生成文件都留在
# build/app/esp32/esp32_s3_qemu 下，便于清理和分层阅读。
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c_claw_esp32_s3_qemu)
