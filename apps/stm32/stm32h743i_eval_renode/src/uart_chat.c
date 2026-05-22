#include "uart_chat.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "real_llm_smoke.h"

#include <stddef.h>
#include <string.h>

static void prompt(void)
{
    board_uart_write("c-claw stm32 chat> ");
}

static void backspace(char *line, size_t *len)
{
    if (*len == 0) return;
    (*len)--;
    line[*len] = '\0';
    board_uart_write("\b \b");
}

void uart_chat_run(void)
{
    char line[512];
    size_t len = 0;
    memset(line, 0, sizeof(line));

    board_uart_write("\n[chat] UART real LLM chat ready. Press ESC to clear the current line.\n");
    prompt();

    for (;;) {
        char ch;
        if (!board_uart_getc_nonblocking(&ch)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ch == 0x1b) {
            len = 0;
            line[0] = '\0';
            board_uart_write("\n");
            prompt();
            continue;
        }
        if (ch == '\b' || ch == 0x7f) {
            backspace(line, &len);
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            board_uart_write("\n");
            line[len] = '\0';
            if (len > 0) {
                if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
                    board_uart_write("[chat] exit requested; chat task will idle.\n");
                    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (strcmp(line, "/help") == 0) {
                    board_uart_write("commands: /help, /quit. Type any other line to send it to the real LLM.\n");
                } else {
                    board_uart_write("[chat] sending real HTTPS LLM request...\n");
                    (void)real_llm_chat(line);
                }
            }
            len = 0;
            line[0] = '\0';
            prompt();
            continue;
        }

        if (len + 1 < sizeof(line)) {
            line[len++] = ch;
            line[len] = '\0';
            char echo[2] = {ch, '\0'};
            board_uart_write(echo);
        }
    }
}
