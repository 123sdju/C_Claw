/**
 * test_plugin_process_resilience.c
 *
 * POSIX app 层负责进程 worker：这里固定 timeout 和 restartOnCrash 行为。
 * JSON-RPC envelope 已下沉到 core SDK，本测试只验证 process pipe 生命周期。
 */

#include "cc/plugin/cc_plugin_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_restart_once(void)
{
    char state_path[256];
    snprintf(state_path, sizeof(state_path),
        "/tmp/cclaw_plugin_resilience_%ld.state", (long)getpid());
    remove(state_path);

    char *argv[] = {
        "python3",
        "apps/posix/cli/tests/mock_plugin_resilience.py",
        "flaky",
        state_path,
        NULL
    };
    cc_plugin_process_t *process = NULL;
    cc_result_t rc = cc_plugin_process_start_with_options(
        "python3",
        argv,
        1,
        1000,
        &process
    );
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        remove(state_path);
        return 0;
    }

    char *response = NULL;
    rc = cc_plugin_process_call(
        process,
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"probe\",\"params\":{}}",
        &response
    );
    int ok = rc.code == CC_OK && response && strstr(response, "restarted");
    cc_result_free(&rc);
    free(response);
    cc_plugin_process_destroy(process);
    remove(state_path);
    return ok;
}

static int test_timeout(void)
{
    char *argv[] = {
        "python3",
        "apps/posix/cli/tests/mock_plugin_resilience.py",
        "slow",
        NULL
    };
    cc_plugin_process_t *process = NULL;
    cc_result_t rc = cc_plugin_process_start_with_options(
        "python3",
        argv,
        0,
        100,
        &process
    );
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 0;
    }

    char *response = NULL;
    rc = cc_plugin_process_call(
        process,
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"slow\",\"params\":{}}",
        &response
    );
    int ok = rc.code == CC_ERR_TIMEOUT;
    cc_result_free(&rc);
    free(response);
    cc_plugin_process_destroy(process);
    return ok;
}

int main(void)
{
    return (test_restart_once() && test_timeout()) ? 0 : 1;
}
