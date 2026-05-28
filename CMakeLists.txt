cmake_minimum_required(VERSION 3.16)
project(c_claw_sdk VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)



set(CC_PROFILE "core-minimal" CACHE STRING "Build profile: core-minimal")
set_property(CACHE CC_PROFILE PROPERTY STRINGS
    core-minimal
    multimodal-full
    desktop-agent
    mcu-text
    mcu-mm-lite)

add_subdirectory(cclaw)

enable_testing()
add_subdirectory(cclaw/tests)
