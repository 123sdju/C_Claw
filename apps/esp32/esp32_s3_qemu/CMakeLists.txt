# 学习导读：apps/esp32/esp32_s3_qemu/CMakeLists.txt
# 所属层次：ESP32 应用层。
# 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c_claw_esp32_s3_qemu)
