#include "cc_board_tools_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Media bytes stay app-owned: the SDK only transports data_base64 already present
 * in artifacts_json. Keeping file I/O and base64 here avoids pulling codecs,
 * camera formats, or board storage policy into the portable runtime.
 */
int cc_board_read_file_all(const char *path, unsigned char **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *data;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    data = malloc((size_t)size ? (size_t)size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return 0;
}

static char *base64_encode(const unsigned char *data, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = table[triple & 0x3F];
    }
    if (len % 3) {
        out[out_len - 1] = '=';
        if (len % 3 == 1) out[out_len - 2] = '=';
    }
    out[out_len] = '\0';
    return out;
}

char *cc_board_file_base64_if_requested(const char *path, int embed_base64, size_t *out_size)
{
    unsigned char *data = NULL;
    size_t size = 0;
    char *encoded = NULL;
    if (out_size) *out_size = 0;
    if (cc_board_read_file_all(path, &data, &size) != 0) return NULL;
    if (out_size) *out_size = size;
    if (embed_base64) encoded = base64_encode(data, size);
    free(data);
    return encoded;
}

char *cc_board_artifact_json(
    const char *id,
    const char *kind,
    const char *mime,
    const char *path,
    const char *data_base64,
    size_t bytes,
    int width,
    int height,
    int duration_ms
)
{
    cc_json_value_t *arr = cc_json_create_array();
    cc_json_value_t *obj = cc_json_create_object();
    char *created_at = cc_board_now_iso8601();
    cc_json_object_set(obj, "id", cc_json_create_string(id ? id : ""));
    cc_json_object_set(obj, "kind", cc_json_create_string(kind ? kind : "file"));
    cc_json_object_set(obj, "mime", cc_json_create_string(mime ? mime : "application/octet-stream"));
    cc_json_object_set(obj, "path", cc_json_create_string(path ? path : ""));
    if (data_base64) cc_json_object_set(obj, "data_base64", cc_json_create_string(data_base64));
    cc_json_object_set(obj, "bytes", cc_json_create_number((double)bytes));
    if (width > 0) cc_json_object_set(obj, "width", cc_json_create_number(width));
    if (height > 0) cc_json_object_set(obj, "height", cc_json_create_number(height));
    if (duration_ms > 0) cc_json_object_set(obj, "duration_ms", cc_json_create_number(duration_ms));
    cc_json_object_set(obj, "created_at", cc_json_create_string(created_at ? created_at : ""));
    cc_json_array_append(arr, obj);
    char *json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    free(created_at);
    return json;
}
