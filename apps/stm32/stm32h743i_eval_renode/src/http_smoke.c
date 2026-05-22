#include "http_smoke.h"

#include "board.h"
#include "cc/ports/cc_http_client.h"

#include <stdio.h>
#include <string.h>

int http_smoke_run(const char *url)
{
    cc_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = "GET";
    request.url = url;
    request.timeout_ms = 5000;
    request.max_response_bytes = 1024;

    cc_http_response_t response;
    cc_result_t rc = cc_http_client_perform(&request, &response);
    if (rc.code != CC_OK) {
        board_uart_write("[fail] http_get: ");
        board_uart_write(rc.message ? rc.message : cc_error_string(rc.code));
        board_uart_write("\n");
        cc_result_free(&rc);
        return 0;
    }

    char line[96];
    snprintf(line, sizeof(line), "[pass] http_get status=%ld bytes=%lu\n",
        response.status_code, (unsigned long)response.body_size);
    board_uart_write(line);
    if (response.body && strstr(response.body, "cclaw-renode-ok")) {
        board_uart_write("CCLAW_STM32H743_RENODE_HTTP_PASS\n");
    }
    cc_http_response_free(&response);
    return 1;
}
