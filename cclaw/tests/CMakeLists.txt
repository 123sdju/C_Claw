# 学习导读：cclaw/tests/CMakeLists.txt
# 所属层次：测试层。
# 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
function(cc_add_sdk_test test_name source_path)
    add_executable(${test_name} "${source_path}")
    target_link_libraries(${test_name} PRIVATE c_claw_runtime)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

set(C_CLAW_CORE_TESTS
    test_thread_mutex
    test_tool_registry_freeze
    test_logger_concurrent
    test_event_bus_concurrent
    test_runtime_concurrent_sessions
    test_config_missing_defaults
)

foreach(test_name IN LISTS C_CLAW_CORE_TESTS)
    cc_add_sdk_test(${test_name} "core/${test_name}.c")
endforeach()

set(C_CLAW_ADAPTER_TESTS
    test_memory_store_concurrent
    test_tool_executor_policy_approval
    test_message_envelope_serialization
)

foreach(test_name IN LISTS C_CLAW_ADAPTER_TESTS)
    cc_add_sdk_test(${test_name} "adapters/${test_name}.c")
endforeach()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "posix" AND CC_ENABLE_SHELL)
    add_executable(test_process_capture "platform/posix/test_process_capture.c")
    target_link_libraries(test_process_capture PRIVATE c_claw_runtime)
    add_test(NAME process_capture_large_output COMMAND test_process_capture)
    set_tests_properties(process_capture_large_output PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
