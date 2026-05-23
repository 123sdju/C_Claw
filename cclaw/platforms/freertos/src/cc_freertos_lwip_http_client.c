/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_lwip_http_client.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_freertos_lwip_http_client.c — 基于 lwIP 裸 socket 的 HTTP 客户端实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 FreeRTOS 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_http_client.h 端口接口在裸 FreeRTOS + lwIP 环境的具体实现，
 *   不使用 libcurl 或任何第三方 HTTP 库，直接从 TCP socket 构造 HTTP/1.0 请求
 *   并解析响应。不依赖文件系统、进程或动态加载。向上层提供统一的
 *   cc_http_client_perform() / cc_http_response_free() 接口。
 *
 * 核心架构（纯 lwIP socket + 手动 HTTP 构造）：
 *   1. parse_url() — 手动解析 HTTP/HTTPS URL（scheme、host、port、path）
 *   2. resolve_ipv4() — IPv4 地址解析，优先 inet_addr()，回退 lwip_getaddrinfo()
 *   3. 构造 HTTP/1.0 请求头（Connection: close，禁止 keep-alive）
 *   4. transport_write_all() — 循环发送直到全部数据发出
 *   5. transport_read() — 接收原始 HTTP 响应，累积到动态缓冲区
 *   6. 手动解析响应：查找 "\r\n\r\n" 分割头部和 body，提取状态码
 *   7. 支持流式回调：若 request->on_body 非空，一次性回调整个 body
 *
 * TLS 支持（可选，基于 mbedTLS）：
 *   由 CCLAW_FREERTOS_ENABLE_MBEDTLS 编译宏控制。启用后：
 *     - start_tls() 执行 mbedTLS 握手（VERIFY_NONE，使用 xorshift RNG）
 *     - tls_send / tls_recv 将 mbedTLS 直接绑定到 lwIP socket 的 BIO
 *     - 支持 ECDHE-RSA/ECDSA + AES-GCM 密码套件
 *
 * 限制与设计决策：
 *   - HTTP/1.0 协议：不处理 chunked encoding 或 keep-alive
 *   - 不支持取消令牌：cancel_token 字段未在实现中使用
 *   - 不支持流式 SSE：流式回调在完整响应接收后一次性调用
 *   - 无 header 解析：响应头被丢弃，仅提取状态码和 body
 *   - 仅 IPv4：不支持 IPv6
 *   - 缓冲区：header 构造 768 字节，读取分块 512 字节
 *   - 超时：通过 lwip_setsockopt SO_RCVTIMEO/SO_SNDTIMEO 设置 socket 超时
 */

#include "cc/ports/cc_http_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#endif

typedef struct parsed_url {
    char scheme[6];
    char host[128];
    char path[256];
    uint16_t port;
    int use_tls;
} parsed_url_t;

typedef struct transport {
    int fd;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    uint32_t rng_state;
    int tls_active;
#endif
} transport_t;

static const char *request_header_value(const cc_http_request_t *request, const char *name)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (request->headers[i].name && request->headers[i].value &&
            strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

static char *cc_strdup_len(const char *data, size_t len)
{
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

static int append_bytes(char **buf, size_t *len, size_t max_len, const char *data, size_t data_len)
{
    if (max_len > 0 && *len + data_len > max_len) return 0;
    char *next = (char *)realloc(*buf, *len + data_len + 1);
    if (!next) return 0;
    memcpy(next + *len, data, data_len);
    *len += data_len;
    next[*len] = '\0';
    *buf = next;
    return 1;
}

static cc_result_t parse_url(const char *url, parsed_url_t *out)
{
    if (!url) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL is missing");

    memset(out, 0, sizeof(*out));
    const char *prefix_end = strstr(url, "://");
    if (!prefix_end) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL scheme is missing");
    }

    size_t scheme_len = (size_t)(prefix_end - url);
    if (scheme_len >= sizeof(out->scheme)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL scheme is too long");
    }
    for (size_t i = 0; i < scheme_len; i++) {
        out->scheme[i] = (char)tolower((unsigned char)url[i]);
    }
    out->scheme[scheme_len] = '\0';

    if (strcmp(out->scheme, "http") == 0) {
        out->port = 80;
        out->use_tls = 0;
    } else if (strcmp(out->scheme, "https") == 0) {
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
        out->port = 443;
        out->use_tls = 1;
#else
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "FreeRTOS lwIP HTTP client was built without HTTPS support");
#endif
    } else {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "FreeRTOS lwIP HTTP client only supports http:// and https:// URLs");
    }

    const char *cursor = prefix_end + 3;
    const char *slash = strchr(cursor, '/');
    const char *host_end = slash ? slash : cursor + strlen(cursor);
    const char *colon = memchr(cursor, ':', (size_t)(host_end - cursor));

    size_t host_len = (size_t)((colon ? colon : host_end) - cursor);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL host is missing or too long");
    }
    memcpy(out->host, cursor, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL port is invalid");
        }
        out->port = (uint16_t)port;
    }

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof(out->path)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL path is too long");
    }
    strcpy(out->path, path);
    return cc_result_ok();
}

static cc_result_t resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = PP_HTONS(port);

    out->sin_addr.s_addr = inet_addr(host);
    if (out->sin_addr.s_addr != IPADDR_NONE) {
        return cc_result_ok();
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *results = NULL;
    int rc = lwip_getaddrinfo(host, NULL, &hints, &results);
    if (rc != 0 || !results) {
        return cc_result_error(CC_ERR_NETWORK, "lwIP DNS resolution failed");
    }

    struct sockaddr_in *resolved = (struct sockaddr_in *)results->ai_addr;
    out->sin_addr = resolved->sin_addr;
    lwip_freeaddrinfo(results);
    return cc_result_ok();
}

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
static int test_rng(void *ctx, unsigned char *out, size_t len)
{
    uint32_t *state = (uint32_t *)ctx;
    for (size_t i = 0; i < len; i++) {
        uint32_t x = *state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *state = x;
        out[i] = (unsigned char)(x & 0xffu);
    }
    return 0;
}

static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int sent = lwip_send(fd, buf, len, 0);
    return sent < 0 ? -1 : sent;
}

static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int got = lwip_recv(fd, buf, len, 0);
    return got < 0 ? -1 : got;
}

static cc_result_t start_tls(transport_t *transport, const char *tls_host)
{
    static const int suites[] = {
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        0
    };

    mbedtls_ssl_init(&transport->ssl);
    mbedtls_ssl_config_init(&transport->conf);
    transport->rng_state = 0xc0ffeeu;

    int rc = mbedtls_ssl_config_defaults(&transport->conf,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS config defaults failed");

    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&transport->conf, test_rng, &transport->rng_state);
    mbedtls_ssl_conf_ciphersuites(&transport->conf, suites);

    rc = mbedtls_ssl_setup(&transport->ssl, &transport->conf);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS ssl setup failed");

    rc = mbedtls_ssl_set_hostname(&transport->ssl, tls_host);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS SNI setup failed");

    mbedtls_ssl_set_bio(&transport->ssl, &transport->fd, tls_send, tls_recv, NULL);
    do {
        rc = mbedtls_ssl_handshake(&transport->ssl);
    } while (0);

    if (rc != 0) {
        char msg[96];
        char err[64];
        mbedtls_strerror(rc, err, sizeof(err));
        snprintf(msg, sizeof(msg), "mbedTLS handshake failed: %s", err);
        return cc_result_error(CC_ERR_NETWORK, msg);
    }

    transport->tls_active = 1;
    return cc_result_ok();
}
#endif

static void transport_close(transport_t *transport)
{
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        (void)mbedtls_ssl_close_notify(&transport->ssl);
    }
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_config_free(&transport->conf);
#endif
    if (transport->fd >= 0) lwip_close(transport->fd);
    transport->fd = -1;
}

static cc_result_t transport_write_all(transport_t *transport, const char *data, size_t len)
{
    while (len > 0) {
        int sent;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
        if (transport->tls_active) {
            do {
                sent = mbedtls_ssl_write(&transport->ssl, (const unsigned char *)data, len);
            } while (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE);
        } else
#endif
        {
            sent = lwip_send(transport->fd, data, len, 0);
        }
        if (sent <= 0) return cc_result_error(CC_ERR_NETWORK, "lwIP transport send failed");
        data += sent;
        len -= (size_t)sent;
    }
    return cc_result_ok();
}

static int transport_read(transport_t *transport, char *buf, size_t len)
{
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        int got;
        do {
            got = mbedtls_ssl_read(&transport->ssl, (unsigned char *)buf, len);
        } while (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return got;
    }
#endif
    return lwip_recv(transport->fd, buf, len, 0);
}

cc_result_t cc_http_client_perform(const cc_http_request_t *request, cc_http_response_t *out_response)
{
    if (!request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    memset(out_response, 0, sizeof(*out_response));

    parsed_url_t url;
    cc_result_t rc = parse_url(request->url, &url);
    if (rc.code != CC_OK) return rc;
    const char *host_header = request_header_value(request, "Host");
    if (!host_header) host_header = url.host;

    transport_t transport;
    memset(&transport, 0, sizeof(transport));
    transport.fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (transport.fd < 0) return cc_result_error(CC_ERR_NETWORK, "lwIP socket creation failed");

    if (request->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = request->timeout_ms / 1000;
        tv.tv_usec = (request->timeout_ms % 1000) * 1000;
        (void)lwip_setsockopt(transport.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)lwip_setsockopt(transport.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in addr;
    rc = resolve_ipv4(url.host, url.port, &addr);
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    if (lwip_connect(transport.fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        transport_close(&transport);
        return cc_result_error(CC_ERR_NETWORK, "lwIP connect failed");
    }

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (url.use_tls) {
        rc = start_tls(&transport, host_header);
        if (rc.code != CC_OK) {
            transport_close(&transport);
            return rc;
        }
    }
#endif

    const char *method = request->method ? request->method : "GET";
    char header[768];
    int n = snprintf(header, sizeof(header),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
        method, url.path, host_header);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        transport_close(&transport);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP request line too long");
    }
    rc = transport_write_all(&transport, header, (size_t)n);
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    for (size_t i = 0; i < request->header_count; i++) {
        if (!request->headers[i].name || !request->headers[i].value) continue;
        if (strcasecmp(request->headers[i].name, "Host") == 0) continue;
        n = snprintf(header, sizeof(header), "%s: %s\r\n",
            request->headers[i].name, request->headers[i].value);
        if (n < 0 || (size_t)n >= sizeof(header)) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP header too long");
        }
        rc = transport_write_all(&transport, header, (size_t)n);
        if (rc.code != CC_OK) {
            transport_close(&transport);
            return rc;
        }
    }

    size_t body_len = request->body ? strlen(request->body) : 0;
    if (body_len > 0) {
        n = snprintf(header, sizeof(header), "Content-Length: %lu\r\n\r\n", (unsigned long)body_len);
        if (n < 0 || (size_t)n >= sizeof(header)) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP content header too long");
        }
        rc = transport_write_all(&transport, header, (size_t)n);
        if (rc.code == CC_OK) rc = transport_write_all(&transport, request->body, body_len);
    } else {
        rc = transport_write_all(&transport, "\r\n", 2);
    }
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    char *raw = NULL;
    size_t raw_len = 0;
    char chunk[512];
    for (;;) {
        int got = transport_read(&transport, chunk, sizeof(chunk));
        if (got < 0) {
            transport_close(&transport);
            free(raw);
            return cc_result_error(CC_ERR_NETWORK, "lwIP transport receive failed");
        }
        if (got == 0) break;
        if (!append_bytes(&raw, &raw_len, request->max_response_bytes, chunk, (size_t)got)) {
            transport_close(&transport);
            free(raw);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "HTTP response body could not be buffered");
        }
    }
    transport_close(&transport);

    if (!raw) raw = cc_strdup_len("", 0);
    if (!raw) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP response");

    char *body = strstr(raw, "\r\n\r\n");
    if (strncmp(raw, "HTTP/", 5) == 0) {
        char *status = strchr(raw, ' ');
        if (status) out_response->status_code = strtol(status + 1, NULL, 10);
    }
    if (body) {
        body += 4;
        out_response->body_size = raw_len - (size_t)(body - raw);
        out_response->body = cc_strdup_len(body, out_response->body_size);
        free(raw);
        if (!out_response->body) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP body");
    } else {
        out_response->body = raw;
        out_response->body_size = raw_len;
    }

    if (request->on_body && out_response->body_size > 0) {
        return request->on_body(out_response->body, out_response->body_size, request->user_data);
    }
    return cc_result_ok();
}

void cc_http_response_free(cc_http_response_t *response)
{
    if (!response) return;
    for (size_t i = 0; i < response->header_count; i++) {
        free((char *)response->headers[i].name);
        free((char *)response->headers[i].value);
    }
    free(response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
}
