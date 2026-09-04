#include "mcp.h"
#include "handlers.h"
#include <apiserver/config.h>
#include <apiserver/logger.h>
#include <string.h>
#include <sodium.h>
#include <event2/buffer.h>
#include <json-c/json.h>
#include <inttypes.h>

// HTTP status codes are provided by event2/http.h

// Helper to send a JSON-RPC error
static void send_jsonrpc_error(struct evbuffer* out_buf, struct json_object* req_id, int code, const char* message) {
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    
    if (req_id != nullptr) {
        json_object_object_add(response, "id", json_object_get(req_id)); // Add ref
    } else {
        json_object_object_add(response, "id", nullptr);
    }
    
    struct json_object* error_obj = json_object_new_object();
    json_object_object_add(error_obj, "code", json_object_new_int(code));
    json_object_object_add(error_obj, "message", json_object_new_string(message));
    json_object_object_add(response, "error", error_obj);
    
    const char* resp_str = json_object_to_json_string(response);
    evbuffer_add(out_buf, resp_str, strlen(resp_str));
    json_object_put(response);
}

// Helper to send an MCP tool error result (isError: true)
static void send_mcp_tool_error(struct evbuffer* out_buf, struct json_object* req_id, const char* message) {
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    if (req_id != nullptr) {
        json_object_object_add(response, "id", json_object_get(req_id));
    }
    
    struct json_object* result = json_object_new_object();
    json_object_object_add(result, "isError", json_object_new_boolean(true));
    
    struct json_object* content_arr = json_object_new_array();
    struct json_object* text_obj = json_object_new_object();
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    json_object_object_add(text_obj, "text", json_object_new_string(message));
    json_object_array_add(content_arr, text_obj);
    
    json_object_object_add(result, "content", content_arr);
    json_object_object_add(response, "result", result);
    
    const char* resp_str = json_object_to_json_string(response);
    evbuffer_add(out_buf, resp_str, strlen(resp_str));
    json_object_put(response);
}

// Helper to send an MCP tool success result
static void send_mcp_tool_result(struct evbuffer* out_buf, struct json_object* req_id, const char* text_content) {
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    if (req_id != nullptr) {
        json_object_object_add(response, "id", json_object_get(req_id));
    }
    
    struct json_object* result = json_object_new_object();
    struct json_object* content_arr = json_object_new_array();
    struct json_object* text_obj = json_object_new_object();
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    json_object_object_add(text_obj, "text", json_object_new_string(text_content));
    json_object_array_add(content_arr, text_obj);
    
    json_object_object_add(result, "content", content_arr);
    json_object_object_add(response, "result", result);
    
    const char* resp_str = json_object_to_json_string(response);
    evbuffer_add(out_buf, resp_str, strlen(resp_str));
    json_object_put(response);
}

static void mcp_handle_tools_list(struct evbuffer* out_buf, struct json_object* req_id) {
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    if (req_id != nullptr) {
        json_object_object_add(response, "id", json_object_get(req_id));
    } else {
        json_object_object_add(response, "id", nullptr);
    }
    
    struct json_object* result = json_object_new_object();
    struct json_object* tools_array = json_object_new_array();
    
    // Define the sysinfo tool
    struct json_object* sysinfo_tool = json_object_new_object();
    json_object_object_add(sysinfo_tool, "name", json_object_new_string("sysinfo"));
    json_object_object_add(sysinfo_tool, "description", json_object_new_string("Retrieve server system information and metrics."));
    
    struct json_object* inputSchema = json_object_new_object();
    json_object_object_add(inputSchema, "type", json_object_new_string("object"));
    
    struct json_object* properties = json_object_new_object();
    struct json_object* apikey_prop = json_object_new_object();
    json_object_object_add(apikey_prop, "type", json_object_new_string("string"));
    json_object_object_add(apikey_prop, "description", json_object_new_string("The API key required to access system metrics"));
    json_object_object_add(properties, "apikey", apikey_prop);
    
    json_object_object_add(inputSchema, "properties", properties);
    
    struct json_object* required_arr = json_object_new_array();
    json_object_array_add(required_arr, json_object_new_string("apikey"));
    json_object_object_add(inputSchema, "required", required_arr);
    
    json_object_object_add(sysinfo_tool, "inputSchema", inputSchema);
    json_object_array_add(tools_array, sysinfo_tool);
    
    json_object_object_add(result, "tools", tools_array);
    json_object_object_add(response, "result", result);
    
    const char* resp_str = json_object_to_json_string(response);
    evbuffer_add(out_buf, resp_str, strlen(resp_str));
    json_object_put(response);
}

static void handle_sysinfo_tool(struct json_object* arguments, struct evbuffer* out_buf, struct json_object* req_id) {
    const char* apikey = nullptr;
    if (arguments != nullptr) {
        struct json_object* apikey_obj = nullptr;
        if (json_object_object_get_ex(arguments, "apikey", &apikey_obj)) {
            apikey = json_object_get_string(apikey_obj);
        }
    }
    
    if (apikey == nullptr) {
        send_mcp_tool_error(out_buf, req_id, "Missing apikey argument");
        return;
    }
    
    // Validate API Key securely
    const char* expected_key = config_get_telemetry_api_key();
    
    char provided_key[MAX_CONFIG_STR] = {0};
    (void)snprintf(provided_key, sizeof(provided_key), "%s", apikey);
    
    int match = sodium_memcmp(provided_key, expected_key, MAX_CONFIG_STR);
    bool valid_len = (provided_key[0] != '\0' && expected_key[0] != '\0');
    sodium_memzero(provided_key, sizeof(provided_key));
    
    if (match != 0 || !valid_len) {
        send_mcp_tool_error(out_buf, req_id, "Unauthorized: Invalid API key");
        return;
    }
    
    // Valid! Generate the JSON string from shared logic
    char sysinfo_json[1024];
    build_sysinfo_json_string(sysinfo_json, sizeof(sysinfo_json));
    
    send_mcp_tool_result(out_buf, req_id, sysinfo_json);
}

static void mcp_handle_tools_call(struct json_object* params, struct evbuffer* out_buf, struct json_object* req_id) {
    const char* name = nullptr;
    struct json_object* arguments = nullptr;
    
    struct json_object* name_obj = nullptr;
    if (json_object_object_get_ex(params, "name", &name_obj)) {
        name = json_object_get_string(name_obj);
    }
    
    json_object_object_get_ex(params, "arguments", &arguments);
    
    if (name == nullptr) {
        send_jsonrpc_error(out_buf, req_id, -32602, "Missing tool name");
        return;
    }
    
    if (strcmp(name, "sysinfo") == 0) {
        handle_sysinfo_tool(arguments, out_buf, req_id);
        return;
    }
    
    send_jsonrpc_error(out_buf, req_id, -32601, "Tool not found");
}

void mcp_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf) {
    *out_status = HTTP_OK; // JSON-RPC mostly returns 200 OK unless HTTP failure
    
    if (body == nullptr || json_object_get_type(body) != json_type_object) {
        send_jsonrpc_error(out_buf, nullptr, -32700, "Parse error");
        return;
    }
    
    struct json_object* jsonrpc_obj = nullptr;
    if (!json_object_object_get_ex(body, "jsonrpc", &jsonrpc_obj) || strcmp(json_object_get_string(jsonrpc_obj), "2.0") != 0) {
        send_jsonrpc_error(out_buf, nullptr, -32600, "Invalid Request");
        return;
    }
    
    struct json_object* method_obj = nullptr;
    if (!json_object_object_get_ex(body, "method", &method_obj) || json_object_get_type(method_obj) != json_type_string) {
        struct json_object* req_id = nullptr;
        json_object_object_get_ex(body, "id", &req_id);
        send_jsonrpc_error(out_buf, req_id, -32600, "Invalid Request");
        return;
    }
    
    struct json_object* req_id = nullptr;
    json_object_object_get_ex(body, "id", &req_id);
    
    const char* method = json_object_get_string(method_obj);
    
    if (strcmp(method, "tools/list") == 0) {
        mcp_handle_tools_list(out_buf, req_id);
    } else if (strcmp(method, "tools/call") == 0) {
        struct json_object* params = nullptr;
        if (!json_object_object_get_ex(body, "params", &params) || json_object_get_type(params) != json_type_object) {
            send_jsonrpc_error(out_buf, req_id, -32602, "Invalid params");
            return;
        }
        mcp_handle_tools_call(params, out_buf, req_id);
    } else {
        send_jsonrpc_error(out_buf, req_id, -32601, "Method not found");
    }
}
