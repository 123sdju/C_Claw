#include "cc/app/cc_mcp_runtime_manager.h"

int main(void)
{
    int matches = 0;
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}",
            &matches).code != CC_OK || !matches) {
        return 1;
    }
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}",
            &matches).code != CC_OK || matches) {
        return 1;
    }
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"result\":{}}",
            &matches).code != CC_OK || !matches) {
        return 1;
    }
    return 0;
}
