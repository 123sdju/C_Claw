# 学习导读：apps/posix/cli/CMakeLists.txt
# 所属层次：POSIX CLI 应用层。
# 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
set(C_CLAW_APP_FEATURE_SOURCES
    src/cc_posix_cli_features.c
)

# shell 工具依赖进程执行能力，因此只在 profile/用户显式开启 CC_ENABLE_SHELL 时编译。
if(CC_ENABLE_SHELL)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/tools/cc_shell_run_tool.c
        src/sandbox/cc_local_sandbox.c
    )
endif()

# Docker sandbox 是桌面增强能力，和 local sandbox 分开裁剪，便于 Windows/设备 profile 关闭。
if(CC_ENABLE_DOCKER_SANDBOX)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/sandbox/cc_docker_sandbox.c
    )
endif()

# 插件系统需要子进程 + 管道；ESP32 等无进程模型的平台不会编译这些源文件。
if(CC_ENABLE_PLUGIN)
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/tools/cc_plugin_tool.c
        src/plugin/cc_plugin_process.c
        src/plugin/cc_plugin_manager.c
    )
endif()

# MCP app 层只提供 stdio/HTTP/SSE/streamable HTTP transport factory；
# 协议状态机和 tool bridge 在 core SDK 中。
if(CC_ENABLE_MCP AND (CC_ENABLE_MCP_STDIO OR CC_ENABLE_MCP_HTTP))
    list(APPEND C_CLAW_APP_FEATURE_SOURCES
        src/mcp/cc_mcp_manager.c
    )
endif()

# app_features 只负责把应用特有工厂暴露给 runtime_builder，不直接包含 main。
add_library(c_claw_app_features STATIC ${C_CLAW_APP_FEATURE_SOURCES})
target_include_directories(c_claw_app_features PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
)
target_link_libraries(c_claw_app_features PUBLIC
    c_claw_runtime
)

# CLI 可执行文件是最终产品入口；它链接 runtime 和 app_features 完成依赖组装。
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
