/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_platform.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_platform.h — platform selection and capability defaults.
 *
 * Platform implementations live under cclaw/platforms/<platform>. This header only
 * exposes the selected platform id and conservative capability defaults. Build
 * profiles may override feature macros through compile definitions.
 */

#ifndef CC_PLATFORM_H
#define CC_PLATFORM_H

#define CC_PLATFORM_POSIX   100
#define CC_PLATFORM_WINDOWS 200
#define CC_PLATFORM_ESP32   300
#define CC_PLATFORM_FREERTOS 400

#ifndef CC_PLATFORM
#  if defined(__XTENSA__) || defined(ESP_PLATFORM)
#    define CC_PLATFORM CC_PLATFORM_ESP32
#  elif defined(CC_FREERTOS_PLATFORM)
#    define CC_PLATFORM CC_PLATFORM_FREERTOS
#  elif defined(_WIN32)
#    define CC_PLATFORM CC_PLATFORM_WINDOWS
#  else
#    define CC_PLATFORM CC_PLATFORM_POSIX
#  endif
#endif

#if CC_PLATFORM == CC_PLATFORM_POSIX
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 1
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 1
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 1
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 1
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 1
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 1
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 1
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 1
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 1
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 1
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 4096
#  define CC_LIMIT_HEAP_WARN_KB 0
#  define CC_LIMIT_STACK_SIZE_KB 1024
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 100
#elif CC_PLATFORM == CC_PLATFORM_WINDOWS
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 1
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 1
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 1
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 0
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 0
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 1
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 1
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 260
#  define CC_LIMIT_HEAP_WARN_KB 0
#  define CC_LIMIT_STACK_SIZE_KB 1024
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 100
#elif CC_PLATFORM == CC_PLATFORM_ESP32
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 0
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 0
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 0
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 1
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 1
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 0
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 0
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 256
#  define CC_LIMIT_HEAP_WARN_KB 400
#  define CC_LIMIT_STACK_SIZE_KB 16
#  define CC_LIMIT_HTTP_RESP_MAX 32768
#  define CC_LIMIT_SESSION_HISTORY 10
#elif CC_PLATFORM == CC_PLATFORM_FREERTOS
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 0
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 0
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 0
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 0
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 0
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 0
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 0
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 0
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 0
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 0
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 0
#  endif
#  define CC_LIMIT_MAX_PATH 256
#  define CC_LIMIT_HEAP_WARN_KB 256
#  define CC_LIMIT_STACK_SIZE_KB 16
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 8
#else
#  error "Unknown CC_PLATFORM value"
#endif

#ifndef CC_TOOL_SHELL_RUN
#  define CC_TOOL_SHELL_RUN 0
#endif
#ifndef CC_SANDBOX_LOCAL
#  define CC_SANDBOX_LOCAL 0
#endif
#ifndef CC_SANDBOX_DOCKER
#  define CC_SANDBOX_DOCKER 0
#endif
#ifndef CC_TOOL_HTTP_REQUEST
#  define CC_TOOL_HTTP_REQUEST 0
#endif
#ifndef CC_TOOL_FILE_READ
#  define CC_TOOL_FILE_READ 1
#endif
#ifndef CC_TOOL_FILE_WRITE
#  define CC_TOOL_FILE_WRITE 1
#endif
#ifndef CC_TOOL_PLUGIN
#  define CC_TOOL_PLUGIN 0
#endif
#ifndef CC_STORAGE_SQLITE
#  define CC_STORAGE_SQLITE 0
#endif
#ifndef CC_STORAGE_JSON_FILE
#  define CC_STORAGE_JSON_FILE 1
#endif
#ifndef CC_STORAGE_MEMORY
#  define CC_STORAGE_MEMORY 1
#endif
#ifndef CC_LLM_OPENAI
#  define CC_LLM_OPENAI 0
#endif
#ifndef CC_LLM_OLLAMA
#  define CC_LLM_OLLAMA 0
#endif
#ifndef CC_LLM_ANTHROPIC
#  define CC_LLM_ANTHROPIC 0
#endif
#ifndef CC_HAS_MEMORY
#  define CC_HAS_MEMORY 1
#endif
#ifndef CC_GATEWAY_CLI
#  define CC_GATEWAY_CLI 0
#endif
#ifndef CC_GATEWAY_UART
#  define CC_GATEWAY_UART 0
#endif

#include "cc/ports/cc_platform_check.h"

#endif
