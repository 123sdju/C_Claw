# 学习导读：cclaw/ports/CMakeLists.txt
# 所属层次：端口层。
# 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
add_library(c_claw_ports_headers INTERFACE)
target_include_directories(c_claw_ports_headers INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
)
