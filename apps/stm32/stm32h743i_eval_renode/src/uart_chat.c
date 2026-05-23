#include "uart_chat.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "board_storage.h"
#include "cc/ports/cc_filesystem.h"
#include "ff.h"
#include "real_llm_smoke.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
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

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void print_fs_error(cc_result_t rc)
{
    board_uart_write("[fs] ");
    board_uart_write(rc.message ? rc.message : cc_error_string(rc.code));
    board_uart_write("\n");
    cc_result_free(&rc);
}

static void print_fresult(const char *op, FRESULT fr)
{
    char line[96];
    snprintf(line, sizeof(line), "[fsdebug] %s FatFs=%u\n", op, (unsigned)fr);
    board_uart_write(line);
}

static void raw_list_dir(const char *path)
{
    DIR dir;
    FILINFO info;
    FRESULT fr = f_opendir(&dir, path);
    print_fresult(path, fr);
    if (fr != FR_OK) return;
    for (;;) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') break;
        board_uart_write("[fsdebug]   ");
        board_uart_write(info.fname);
        if (info.fattrib & AM_DIR) board_uart_write("/");
        board_uart_write("\n");
    }
    f_closedir(&dir);
}

static void command_fsdebug(void)
{
    if (!board_storage_is_mounted() && !board_storage_mount()) return;
    print_fresult("stat 0:/", f_stat("0:/", &(FILINFO){0}));
    raw_list_dir("0:/");
}

static void workspace_path(char *out, size_t out_len, const char *arg)
{
    arg = skip_spaces(arg);
    while (*arg == '/') arg++;
    if (*arg) {
        snprintf(out, out_len, "%s/%s", CCLAW_STM32H743_WORKSPACE, arg);
    } else {
        snprintf(out, out_len, "%s", CCLAW_STM32H743_WORKSPACE);
    }
}

static void command_ls(const char *arg)
{
    if (!board_storage_is_mounted() && !board_storage_mount()) return;

    char path[256];
    workspace_path(path, sizeof(path), arg);

    cc_filesystem_t fs;
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
        return;
    }

    char **items = NULL;
    size_t count = 0;
    rc = fs.vtable->make_dir(fs.self, CCLAW_STM32H743_WORKSPACE);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
    }
    rc = fs.vtable->list_dir(fs.self, path, &items, &count);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
        board_uart_write("[fs] running raw FatFs debug for /ls failure\n");
        command_fsdebug();
        fs.vtable->destroy(fs.self);
        return;
    }
    if (rc.code != CC_OK) {
        print_fs_error(rc);
    } else {
        for (size_t i = 0; i < count; i++) {
            board_uart_write(items[i]);
            board_uart_write("\n");
            free(items[i]);
        }
        free(items);
        if (count == 0) board_uart_write("[fs] empty\n");
    }
    fs.vtable->destroy(fs.self);
}

static void command_cat(const char *arg)
{
    if (!board_storage_is_mounted() && !board_storage_mount()) return;

    char path[256];
    workspace_path(path, sizeof(path), arg);

    cc_filesystem_t fs;
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
        return;
    }

    char *text = NULL;
    rc = fs.vtable->read_text(fs.self, path, &text);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
    } else {
        board_uart_write(text ? text : "");
        if (!text || text[strlen(text) ? strlen(text) - 1 : 0] != '\n') board_uart_write("\n");
        free(text);
    }
    fs.vtable->destroy(fs.self);
}

static void command_write(const char *arg)
{
    if (!board_storage_is_mounted() && !board_storage_mount()) return;

    arg = skip_spaces(arg);
    const char *space = strchr(arg, ' ');
    if (!space) {
        board_uart_write("usage: /write <path> <text>\n");
        return;
    }

    char rel[128];
    size_t rel_len = (size_t)(space - arg);
    if (rel_len >= sizeof(rel)) rel_len = sizeof(rel) - 1;
    memcpy(rel, arg, rel_len);
    rel[rel_len] = '\0';

    char path[256];
    workspace_path(path, sizeof(path), rel);
    const char *text = skip_spaces(space);

    cc_filesystem_t fs;
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
        return;
    }

    rc = fs.vtable->write_text(fs.self, path, text);
    if (rc.code != CC_OK) {
        print_fs_error(rc);
    } else {
        board_uart_write("[fs] wrote ");
        board_uart_write(path);
        board_uart_write("\n");
    }
    fs.vtable->destroy(fs.self);
}

static void command_mode_not_supported(const char *name)
{
    board_uart_write("[chat] ");
    board_uart_write(name);
    board_uart_write(" is not a local STM32 UART mode yet; request it in normal text if you want the model to discuss it.\n");
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
                    board_uart_write("commands: /help, /quit, /ls [path], /cat <path>, /write <path> <text>, /fsdebug. Type any non-command line to send it to the real LLM.\n");
                } else if (strncmp(line, "/ls", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
                    command_ls(line + 3);
                } else if (strcmp(line, "/fsdebug") == 0) {
                    command_fsdebug();
                } else if (strncmp(line, "/cat", 4) == 0 && (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
                    command_cat(line + 4);
                } else if (strncmp(line, "/write", 6) == 0 && (line[6] == '\0' || line[6] == ' ' || line[6] == '\t')) {
                    command_write(line + 6);
                } else if (strncmp(line, "/stream", 7) == 0 && (line[7] == '\0' || line[7] == ' ' || line[7] == '\t')) {
                    command_mode_not_supported("/stream");
                } else if (strncmp(line, "/thinking", 9) == 0 && (line[9] == '\0' || line[9] == ' ' || line[9] == '\t')) {
                    command_mode_not_supported("/thinking");
                } else if (line[0] == '/') {
                    board_uart_write("[chat] unknown local command. Type /help.\n");
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
