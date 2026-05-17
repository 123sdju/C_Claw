# 学习导读：cclaw/adapters/CMakeLists.txt
# 所属层次：适配器层。
# 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
set(C_CLAW_ADAPTER_SOURCES
    src/storage/cc_storage_factory.c
    src/storage/cc_json_file_store.c
    src/storage/cc_memory_session_store.c
    src/storage/cc_json_file_memory_store.c
    src/storage/cc_inmem_memory_store.c
    src/storage/cc_memory_store_factory.c
    src/sandbox/cc_policy_engine_impl.c
)

# SQLite 源码作为 vendor 编进适配器对象库；禁用时仍保留 JSON/Memory 后端。
if(CC_ENABLE_SQLITE)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        vendor/sqlite/sqlite3.c
        src/storage/cc_sqlite_session_store.c
        src/storage/cc_sqlite_memory_store.c
    )
    set_source_files_properties(vendor/sqlite/sqlite3.c PROPERTIES
        COMPILE_OPTIONS "-Wno-implicit-fallthrough")
endif()

# 通用工具按功能开关裁剪，避免设备 profile 编入无用能力。
if(CC_ENABLE_HTTP_TOOL)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/tools/common/cc_http_request_tool.c)
endif()

if(CC_ENABLE_FILE_TOOLS)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        src/tools/common/cc_file_read_tool.c
        src/tools/common/cc_file_write_tool.c
    )
endif()

if(CC_ENABLE_MEMORY)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/tools/common/cc_memory_tool.c)
endif()

# HTTP LLM provider 是公共传输层，OpenAI/Ollama/Anthropic 只是协议策略。
if(CC_ENABLE_OPENAI OR CC_ENABLE_OLLAMA OR CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_http_llm_provider.c)
endif()
if(CC_ENABLE_OPENAI)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_openai_provider.c)
endif()
if(CC_ENABLE_OLLAMA)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_ollama_provider.c)
endif()
if(CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_anthropic_provider.c)
endif()

# headers target 暴露 public include；objects target 承载可裁剪的具体实现。
add_library(c_claw_adapters_headers INTERFACE)
target_include_directories(c_claw_adapters_headers INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/sqlite"
)

add_library(c_claw_adapters_objects OBJECT ${C_CLAW_ADAPTER_SOURCES})
target_link_libraries(c_claw_adapters_objects PRIVATE
    c_claw_adapters_headers
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)
