#pragma once
#include <json-c/json.h>
#include <event2/http.h>

/**
 * \file mcp.h
 * \brief MCP stateless HTTP endpoint handlers
 */

/** \brief Handles /mcp requests using JSON-RPC 2.0 and MCP tool definitions. */
void mcp_handler(struct json_object* body, int* out_status, struct evbuffer* out_buf);
