#include "cc_board_tools_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

char *cc_board_strdup(const char *text)
{
    return strdup(text ? text : "");
}

const char *cc_board_json_string_or(const cc_json_value_t *obj, const char *key, const char *fallback)
{
    const char *value = cc_json_string_value(cc_json_object_get(obj, key));
    return (value && *value) ? value : fallback;
}

int cc_board_json_int_or(const cc_json_value_t *obj, const char *key, int fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_int_value(value) : fallback;
}

int cc_board_json_bool_or(const cc_json_value_t *obj, const char *key, int fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_bool_value(value) : fallback;
}

char *cc_board_now_id(const char *prefix)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s_%ld", prefix ? prefix : "media", (long)time(NULL));
    return cc_board_strdup(buf);
}

char *cc_board_now_iso8601(void)
{
    time_t t = time(NULL);
    struct tm tm_info;
    char buf[64];
    localtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    return cc_board_strdup(buf);
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;
    if (!path || !*path) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

static char *board_media_dir(const cc_tool_context_t *ctx)
{
    const char *root = (ctx && ctx->workspace_dir) ? ctx->workspace_dir : ".";
    size_t len = strlen(root) + strlen("/media/board") + 1;
    char *dir = malloc(len);
    if (!dir) return NULL;
    snprintf(dir, len, "%s/media/board", root);
    if (mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }
    return dir;
}

char *cc_board_output_path(
    const cc_tool_context_t *ctx,
    const cc_json_value_t *args,
    const char *key,
    const char *prefix,
    const char *ext
)
{
    const char *requested = cc_json_string_value(cc_json_object_get(args, key));
    if (requested && requested[0] == '/') return cc_board_strdup(requested);

    char *dir = board_media_dir(ctx);
    if (!dir) return NULL;
    char *id = cc_board_now_id(prefix);
    const char *name = (requested && *requested) ? requested : id;
    size_t len = strlen(dir) + 1 + strlen(name) + strlen(ext) + 2;
    char *path = malloc(len);
    if (path) {
        if (requested && *requested) snprintf(path, len, "%s/%s", dir, requested);
        else snprintf(path, len, "%s/%s.%s", dir, id, ext);
    }
    free(id);
    free(dir);
    return path;
}

void cc_board_set_error(cc_tool_result_t *out_result, const char *message)
{
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 0;
    out_result->error = cc_board_strdup(message ? message : "board tool failed");
}

void cc_board_set_success_json(cc_tool_result_t *out_result, char *content_json)
{
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->content = content_json ? content_json : cc_board_strdup("{}");
}
