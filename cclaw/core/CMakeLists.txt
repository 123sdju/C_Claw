# 学习导读：cclaw/core/CMakeLists.txt
# 所属层次：核心层。
# 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
set(C_CLAW_CORE_SOURCES
    vendor/cjson/cJSON.c
    src/core/cc_result.c
    src/core/cc_message.c
    src/core/cc_session.c
    src/core/cc_tool_call.c
    src/core/cc_tool_registry.c
    src/core/cc_event_bus.c
    src/core/cc_memory_entry.c
    src/core/cc_memory_store.c
    src/util/cc_string_builder.c
    src/util/cc_memory.c
    src/util/cc_json.c
    src/util/cc_config.c
    src/util/cc_token_counter.c
    src/util/cc_logger.c
    src/app/cc_context_builder.c
    src/app/cc_tool_executor.c
    src/app/cc_session_manager.c
    src/app/cc_agent_runtime.c
    src/app/cc_runtime_builder.c
    src/app/cc_memory_context.c
)

add_library(c_claw_core_headers INTERFACE)
target_include_directories(c_claw_core_headers INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/cjson"
)

add_library(c_claw_core_objects OBJECT ${C_CLAW_CORE_SOURCES})
target_link_libraries(c_claw_core_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)

add_library(c_claw_core STATIC $<TARGET_OBJECTS:c_claw_core_objects>)
target_link_libraries(c_claw_core PUBLIC
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)
