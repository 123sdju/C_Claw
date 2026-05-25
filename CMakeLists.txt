cmake_minimum_required(VERSION 3.16)
project(c_claw_sdk C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The SDK branch builds only the reusable runtime library and SDK tests.
# Downstream applications should add this repository or cclaw/ as source and
# provide their own gateway plus cc_runtime_feature_set_t implementation.
set(CC_PROFILE "core-minimal" CACHE STRING "Build profile: core-minimal")
set_property(CACHE CC_PROFILE PROPERTY STRINGS core-minimal)

add_subdirectory(cclaw)

enable_testing()
add_subdirectory(cclaw/tests)
