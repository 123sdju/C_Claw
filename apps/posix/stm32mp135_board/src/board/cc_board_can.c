#include "cc_board_tools_internal.h"

#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned int json_can_id(const cc_json_value_t *args)
{
    cc_json_value_t *idv = cc_json_object_get(args, "id");
    const char *ids = cc_json_string_value(idv);
    if (ids && *ids) return (unsigned int)strtoul(ids, NULL, 0);
    return (unsigned int)cc_json_int_value(idv);
}

static int parse_can_data(const char *text, unsigned char data[8])
{
    int count = 0;
    const char *p = text;
    while (p && *p && count < 8) {
        while (*p == ' ' || *p == ':' || *p == '-' || *p == ',') p++;
        if (!*p) break;
        char byte_text[3] = {0, 0, 0};
        if (!p[1]) return -1;
        byte_text[0] = p[0];
        byte_text[1] = p[1];
        char *end = NULL;
        long value = strtol(byte_text, &end, 16);
        if (!end || *end || value < 0 || value > 255) return -1;
        data[count++] = (unsigned char)value;
        p += 2;
    }
    return count;
}

static cc_result_t board_can_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    (void)ctx;
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json && *args_json ? args_json : "{}", &args);
    if (rc.code != CC_OK) {
        cc_board_set_error(out_result, "Invalid CAN arguments JSON");
        return cc_result_ok();
    }
    const char *op = cc_board_json_string_or(args, "operation", "status");
    const char *iface = cc_board_json_string_or(args, "iface", "can0");
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Failed to open SocketCAN raw socket");
        return cc_result_ok();
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        cc_json_destroy(args);
        cc_board_set_error(out_result, "CAN interface not found");
        return cc_result_ok();
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Failed to bind CAN interface");
        return cc_result_ok();
    }

    cc_json_value_t *content = cc_json_create_object();
    cc_json_object_set(content, "ok", cc_json_create_bool(1));
    cc_json_object_set(content, "iface", cc_json_create_string(iface));
    cc_json_object_set(content, "operation", cc_json_create_string(op));
    if (strcmp(op, "send") == 0) {
        struct can_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = json_can_id(args);
        const char *data_text = cc_json_string_value(cc_json_object_get(args, "data"));
        int len = parse_can_data(data_text ? data_text : "", frame.data);
        if (frame.can_id > CAN_EFF_MASK || len < 0 || len > 8) {
            close(fd);
            cc_json_destroy(content);
            cc_json_destroy(args);
            cc_board_set_error(out_result, "Invalid CAN id or data");
            return cc_result_ok();
        }
        frame.can_dlc = (unsigned char)len;
        if (write(fd, &frame, sizeof(frame)) != sizeof(frame)) {
            close(fd);
            cc_json_destroy(content);
            cc_json_destroy(args);
            cc_board_set_error(out_result, "Failed to send CAN frame");
            return cc_result_ok();
        }
        cc_json_object_set(content, "id", cc_json_create_number(frame.can_id));
        cc_json_object_set(content, "dlc", cc_json_create_number(frame.can_dlc));
    } else if (strcmp(op, "read") == 0) {
        int timeout_ms = cc_board_json_int_or(args, "timeout_ms", 1000);
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
            struct can_frame frame;
            if (read(fd, &frame, sizeof(frame)) == sizeof(frame)) {
                char data_hex[3 * 8 + 1];
                char *p = data_hex;
                for (int i = 0; i < frame.can_dlc; ++i) {
                    snprintf(p, 4, "%02X%s", frame.data[i], i + 1 < frame.can_dlc ? " " : "");
                    p += strlen(p);
                }
                cc_json_object_set(content, "id", cc_json_create_number(frame.can_id));
                cc_json_object_set(content, "dlc", cc_json_create_number(frame.can_dlc));
                cc_json_object_set(content, "data", cc_json_create_string(data_hex));
            }
        } else {
            cc_json_object_set(content, "timeout", cc_json_create_bool(1));
        }
    } else if (strcmp(op, "status") != 0) {
        close(fd);
        cc_json_destroy(content);
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Invalid CAN operation; use status, send, or read");
        return cc_result_ok();
    }
    close(fd);
    cc_board_set_success_json(out_result, cc_json_stringify_unformatted(content));
    cc_json_destroy(content);
    cc_json_destroy(args);
    return cc_result_ok();
}

const cc_board_tool_ops_t cc_board_can_tool_ops = {
    "board.can",
    "Validate and interact with a SocketCAN interface using status, send, or read operations.",
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":[\"status\",\"send\",\"read\"]},\"iface\":{\"type\":\"string\"},\"id\":{\"oneOf\":[{\"type\":\"integer\"},{\"type\":\"string\"}]},\"data\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"operation\"]}",
    board_can_call,
    NULL
};
