#include "real_llm_smoke.h"

#include "board.h"
#include "cc/ports/cc_http_client.h"

#include <stdio.h>
#include <string.h>

#if (defined(CCLAW_STM32H743_ENABLE_REAL_LLM) && CCLAW_STM32H743_ENABLE_REAL_LLM) || \
    (defined(CCLAW_STM32H743_ENABLE_UART_CHAT) && CCLAW_STM32H743_ENABLE_UART_CHAT)
#define CCLAW_STM32H743_REAL_CLIENT_ENABLED 1
#else
#define CCLAW_STM32H743_REAL_CLIENT_ENABLED 0
#endif

#if CCLAW_STM32H743_REAL_CLIENT_ENABLED
#include "cclaw_stm32h743_real_config.h"
#endif

#ifndef CCLAW_STM32H743_REAL_LLM_ITERATIONS
#define CCLAW_STM32H743_REAL_LLM_ITERATIONS 1
#endif

#ifndef CCLAW_STM32H743_REAL_LLM_PROMPT
#define CCLAW_STM32H743_REAL_LLM_PROMPT "Return exactly CCLAW_STM32H743_LLM_PASS"
#endif

#if CCLAW_STM32H743_REAL_CLIENT_ENABLED
static void write_status(const char *prefix, long status, size_t bytes)
{
    char line[128];
    snprintf(line, sizeof(line), "%s status=%ld bytes=%lu\n",
        prefix, status, (unsigned long)bytes);
    board_uart_write(line);
}

static int append_chat_path(char *out, size_t out_len, const char *base_url)
{
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') base_len--;

    const char path[] = "/v1/chat/completions";
    if (base_len + sizeof(path) > out_len) {
        board_uart_write("[fail] real_llm_url_too_long\n");
        return 0;
    }
    memcpy(out, base_url, base_len);
    memcpy(out + base_len, path, sizeof(path));
    return 1;
}

static int json_escape(char *out, size_t out_len, const char *in)
{
    size_t pos = 0;
    while (in && *in) {
        unsigned char ch = (unsigned char)*in++;
        const char *escaped = NULL;
        char tmp[7];
        if (ch == '"' || ch == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)ch;
            tmp[2] = '\0';
            escaped = tmp;
        } else if (ch == '\n') {
            escaped = "\\n";
        } else if (ch == '\r') {
            escaped = "\\r";
        } else if (ch == '\t') {
            escaped = "\\t";
        } else if (ch < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
            escaped = tmp;
        }

        if (escaped) {
            size_t len = strlen(escaped);
            if (pos + len >= out_len) return 0;
            memcpy(out + pos, escaped, len);
            pos += len;
        } else {
            if (pos + 1 >= out_len) return 0;
            out[pos++] = (char)ch;
        }
    }
    if (pos >= out_len) return 0;
    out[pos] = '\0';
    return 1;
}

static void write_json_string_fragment(const char *start)
{
    int escape = 0;
    for (const char *p = start; *p; p++) {
        char ch = *p;
        if (escape) {
            if (ch == 'n') board_uart_write("\n");
            else if (ch == 'r') {
            } else if (ch == 't') board_uart_write("    ");
            else {
                char s[2] = {ch, '\0'};
                board_uart_write(s);
            }
            escape = 0;
            continue;
        }
        if (ch == '\\') {
            escape = 1;
            continue;
        }
        if (ch == '"') break;
        char s[2] = {ch, '\0'};
        board_uart_write(s);
    }
}

static void write_chat_answer(const char *body)
{
    const char *content = body ? strstr(body, "\"content\"") : NULL;
    if (content) content = strchr(content, ':');
    if (content) {
        content++;
        while (*content == ' ' || *content == '\t') content++;
        if (*content == '"') {
            board_uart_write("assistant> ");
            write_json_string_fragment(content + 1);
            board_uart_write("\n");
            return;
        }
    }

    board_uart_write("assistant_raw> ");
    if (body) {
        for (size_t i = 0; body[i] && i < 512; i++) {
            char ch = body[i];
            if (ch == '\r' || ch == '\n') ch = ' ';
            char s[2] = {ch, '\0'};
            board_uart_write(s);
        }
    }
    board_uart_write("\n");
}

static int real_llm_request(const char *prompt, unsigned iteration, int print_answer)
{
    char url[192];
    if (!append_chat_path(url, sizeof(url), CCLAW_REAL_CONNECT_BASE_URL)) return 0;

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", CCLAW_REAL_API_KEY);

    char escaped_prompt[768];
    if (!json_escape(escaped_prompt, sizeof(escaped_prompt), prompt)) {
        board_uart_write("[fail] real_llm_prompt_too_long\n");
        return 0;
    }

    char body[1200];
    int n = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.7,\"max_tokens\":512}",
        CCLAW_REAL_MODEL,
        escaped_prompt);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        board_uart_write("[fail] real_llm_body_too_long\n");
        return 0;
    }

    cc_http_header_t headers[] = {
        {"Host", CCLAW_REAL_HOST},
        {"Content-Type", "application/json"},
        {"Authorization", auth},
    };

    cc_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = "POST";
    request.url = url;
    request.headers = headers;
    request.header_count = sizeof(headers) / sizeof(headers[0]);
    request.body = body;
    request.timeout_ms = 30000;
    request.max_response_bytes = 4096;

    cc_http_response_t response;
    cc_result_t rc = cc_http_client_perform(&request, &response);
    if (rc.code != CC_OK) {
        board_uart_write("[fail] real_llm_request: ");
        board_uart_write(rc.message ? rc.message : cc_error_string(rc.code));
        board_uart_write("\n");
        cc_result_free(&rc);
        return 0;
    }

    write_status("[pass] real_llm", response.status_code, response.body_size);
    int ok = response.status_code >= 200 && response.status_code < 300 &&
        response.body && (!print_answer || response.body_size > 0) &&
        (print_answer || strstr(response.body, "CCLAW_STM32H743_LLM_PASS"));
    if (ok && print_answer) {
        write_chat_answer(response.body);
    }
    if (!ok && response.body) {
        board_uart_write("[info] real_llm_body_prefix=");
        for (size_t i = 0; i < response.body_size && i < 160; i++) {
            char ch = response.body[i];
            if (ch == '\r' || ch == '\n') ch = ' ';
            char s[2] = {ch, '\0'};
            board_uart_write(s);
        }
        board_uart_write("\n");
    }
    cc_http_response_free(&response);

    if (ok && !print_answer) {
        char line[96];
        snprintf(line, sizeof(line), "[pass] real_llm_iteration=%u\n", iteration);
        board_uart_write(line);
    }
    return ok;
}

#if defined(CCLAW_STM32H743_ENABLE_REAL_LLM) && CCLAW_STM32H743_ENABLE_REAL_LLM
static int real_llm_once(unsigned iteration)
{
    return real_llm_request(CCLAW_STM32H743_REAL_LLM_PROMPT, iteration, 0);
}
#endif
#endif

int real_llm_smoke_run(void)
{
#if defined(CCLAW_STM32H743_ENABLE_REAL_LLM) && CCLAW_STM32H743_ENABLE_REAL_LLM
    board_uart_write("[init] real LLM HTTPS smoke start\n");
    board_uart_write("[warn] TLS certificate verification disabled for Renode bring-up\n");

    unsigned iterations = CCLAW_STM32H743_REAL_LLM_ITERATIONS;
    if (iterations == 0) iterations = 1;

    unsigned passed = 0;
    for (unsigned i = 1; i <= iterations; i++) {
        if (real_llm_once(i)) {
            passed++;
            if (i == 1) {
                board_uart_write("CCLAW_STM32H743_RENODE_TLS_PASS\n");
                board_uart_write("CCLAW_STM32H743_RENODE_LLM_PASS\n");
            }
        } else {
            break;
        }
    }

    char line[96];
    snprintf(line, sizeof(line), "[pass] real_llm_summary passed=%u total=%u\n", passed, iterations);
    board_uart_write(line);
    if (passed == iterations) {
        board_uart_write("CCLAW_STM32H743_RENODE_STRESS_PASS\n");
        return 1;
    }
    return 0;
#else
    board_uart_write("[skip] real LLM smoke disabled\n");
    return 1;
#endif
}

int real_llm_chat(const char *prompt)
{
#if CCLAW_STM32H743_REAL_CLIENT_ENABLED
    return real_llm_request(prompt, 0, 1);
#else
    (void)prompt;
    board_uart_write("[fail] real LLM chat disabled\n");
    return 0;
#endif
}
