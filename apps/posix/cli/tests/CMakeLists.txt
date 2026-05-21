# 学习导读：apps/posix/cli/tests/CMakeLists.txt
# 所属层次：POSIX CLI 应用层。
# 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
set(C_CLAW_POSIX_CLI_TEST_LIBS
    c_claw_runtime
    c_claw_app_features
)

if(CC_ENABLE_CLI AND TARGET c-claw)
    add_test(NAME c_claw_help COMMAND c-claw --help)
    set_tests_properties(c_claw_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage:")
endif()

add_executable(test_runtime_builder_tools test_runtime_builder_tools.c)
target_link_libraries(test_runtime_builder_tools PRIVATE ${C_CLAW_POSIX_CLI_TEST_LIBS})
add_test(NAME test_runtime_builder_tools COMMAND test_runtime_builder_tools)
set_tests_properties(test_runtime_builder_tools PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(CC_ENABLE_PLUGIN)
    add_executable(test_plugin_process_serial_call test_plugin_process_serial_call.c)
    target_link_libraries(test_plugin_process_serial_call PRIVATE ${C_CLAW_POSIX_CLI_TEST_LIBS})
    add_test(NAME test_plugin_process_serial_call COMMAND test_plugin_process_serial_call)
    set_tests_properties(test_plugin_process_serial_call PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(test_plugin_process_resilience test_plugin_process_resilience.c)
    target_link_libraries(test_plugin_process_resilience PRIVATE ${C_CLAW_POSIX_CLI_TEST_LIBS})
    add_test(NAME test_plugin_process_resilience COMMAND test_plugin_process_resilience)
    set_tests_properties(test_plugin_process_resilience PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(CC_ENABLE_MCP AND CC_ENABLE_MCP_STDIO)
    add_executable(test_mcp_stdio_tool_bridge test_mcp_stdio_tool_bridge.c)
    target_link_libraries(test_mcp_stdio_tool_bridge PRIVATE ${C_CLAW_POSIX_CLI_TEST_LIBS})
    add_test(NAME test_mcp_stdio_tool_bridge COMMAND test_mcp_stdio_tool_bridge)
    set_tests_properties(test_mcp_stdio_tool_bridge PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(CC_ENABLE_MCP AND CC_ENABLE_MCP_HTTP AND CC_ENABLE_MCP_STDIO)
    add_executable(test_mcp_http_transports test_mcp_http_transports.c)
    target_link_libraries(test_mcp_http_transports PRIVATE ${C_CLAW_POSIX_CLI_TEST_LIBS})
    add_test(NAME test_mcp_http_transports COMMAND test_mcp_http_transports)
    set_tests_properties(test_mcp_http_transports PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
