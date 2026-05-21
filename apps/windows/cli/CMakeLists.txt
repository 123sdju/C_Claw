# 学习导读：apps/windows/cli/CMakeLists.txt
# 所属层次：Windows CLI 应用层。
# 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
set(C_CLAW_APP_FEATURE_SOURCES
    src/cc_windows_cli_features.c
)

# shell 工具在 Windows profile 中通过 CreateProcess 风格的平台进程端口实现。
if(CC_ENABLE_SHELL)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/tools/cc_shell_run_tool.c
        src/sandbox/cc_local_sandbox.c
    )
endif()

# Windows CLI 当前不启用 Docker sandbox；上层 cclaw/CMakeLists.txt 会强制关闭该开关。
if(CC_ENABLE_DOCKER_SANDBOX)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/sandbox/cc_docker_sandbox.c
    )
endif()

# 插件系统复用 JSON-RPC over stdio 协议，但底层管道由 Windows platform process 实现。
if(CC_ENABLE_PLUGIN)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/tools/cc_plugin_tool.c
        src/plugin/cc_plugin_process.c
        src/plugin/cc_plugin_manager.c
    )
endif()

if(CC_ENABLE_MCP AND (CC_ENABLE_MCP_STDIO OR CC_ENABLE_MCP_HTTP))
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        ../../posix/cli/src/mcp/cc_mcp_manager.c
    )
endif()

# app_features 暴露 Windows CLI 的 provider/tool/store 工厂给 runtime_builder。
add_library(c_claw_app_features STATIC ${C_CLAW_APP_FEATURE_SOURCES})
target_include_directories(c_claw_app_features PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../posix/cli/include"
)
target_link_libraries(c_claw_app_features PUBLIC
    c_claw_runtime
)

# CLI 可执行文件只在 CC_ENABLE_CLI 为 ON 时生成，core-minimal 这类 profile 会跳过。
if(CC_ENABLE_CLI)
    add_executable(c-claw
        src/main.c
        src/cc_cli_gateway.c
    )
    target_link_libraries(c-claw PRIVATE
        c_claw_runtime
        c_claw_app_features
    )
    set_target_properties(c-claw PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
