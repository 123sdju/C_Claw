# 学习导读：cclaw/platforms/posix/CMakeLists.txt
# 所属层次：平台层。
# 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
add_library(c_claw_platform_posix_objects OBJECT
    src/cc_posix_filesystem.c
    src/cc_posix_path.c
    src/cc_posix_thread.c
)

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS "")
set(CC_PLATFORM_PUBLIC_LIBRARIES "")

if(CC_NEEDS_HTTP_CLIENT)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(CC_PLATFORM_CURL_LIBRARIES CURL::libcurl)
    else()
        if(CMAKE_CROSSCOMPILING)
            find_library(CC_PLATFORM_CURL_LIBRARY NAMES curl libcurl libcurl.so.4
                HINTS "/home/sdju123/linux_study/tool_lib/curl/install/lib"
                NO_CMAKE_FIND_ROOT_PATH)
            find_path(CC_PLATFORM_CURL_INCLUDE_DIR NAMES curl/curl.h
                HINTS "/home/sdju123/linux_study/tool_lib/curl/install/include"
                      "/home/sdju123/linux_study/tool_lib/curl/include"
                NO_CMAKE_FIND_ROOT_PATH)
        else()
            find_library(CC_PLATFORM_CURL_LIBRARY NAMES curl libcurl libcurl.so.4)
            find_path(CC_PLATFORM_CURL_INCLUDE_DIR NAMES curl/curl.h
                PATHS "${CMAKE_CURRENT_LIST_DIR}/vendor/curl/include")
        endif()
        if(CC_PLATFORM_CURL_LIBRARY AND CC_PLATFORM_CURL_INCLUDE_DIR)
            set(CC_PLATFORM_CURL_LIBRARIES "${CC_PLATFORM_CURL_LIBRARY}")
            if(CMAKE_CROSSCOMPILING)
                foreach(CC_PLATFORM_CURL_DEP
                    "/home/sdju123/linux_study/tool_lib/openssl/libssl.so"
                    "/home/sdju123/linux_study/tool_lib/openssl/libcrypto.so"
                    "/home/sdju123/linux_study/tool_lib/zlib-lib/lib/libz.so"
                )
                    if(EXISTS "${CC_PLATFORM_CURL_DEP}")
                        list(APPEND CC_PLATFORM_CURL_LIBRARIES "${CC_PLATFORM_CURL_DEP}")
                    endif()
                endforeach()
            endif()
        endif()
    endif()

    if(NOT CC_PLATFORM_CURL_LIBRARIES)
        message(FATAL_ERROR
            "POSIX HTTP-backed features are enabled but libcurl was not found.")
    endif()
    target_sources(c_claw_platform_posix_objects PRIVATE src/cc_curl_http_client.c)
    list(APPEND CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_CURL_LIBRARIES})
    if(DEFINED CC_PLATFORM_CURL_INCLUDE_DIR)
        list(APPEND CC_PLATFORM_PUBLIC_INCLUDE_DIRS "${CC_PLATFORM_CURL_INCLUDE_DIR}")
    endif()
endif()

if(CC_ENABLE_SHELL OR CC_ENABLE_PLUGIN OR CC_ENABLE_DOCKER_SANDBOX OR CC_ENABLE_MCP_STDIO)
    target_sources(c_claw_platform_posix_objects PRIVATE src/cc_posix_process.c)
endif()

target_link_libraries(c_claw_platform_posix_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
    Threads::Threads
)

if(CC_PLATFORM_CURL_LIBRARIES)
    target_link_libraries(c_claw_platform_posix_objects PRIVATE ${CC_PLATFORM_CURL_LIBRARIES})
endif()
if(CC_PLATFORM_PUBLIC_INCLUDE_DIRS)
    target_include_directories(c_claw_platform_posix_objects PRIVATE
        ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS}
    )
endif()

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS} PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_PUBLIC_LIBRARIES} PARENT_SCOPE)
