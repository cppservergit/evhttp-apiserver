# Guide: Writing a Well-Behaved Database API Handler in EvHttp

This document is a step-by-step guide for C programmers implementing secure, ultra-high-performance API endpoints in `evhttp-apiserver`. 

The core philosophy of this server relies on **Zero-Allocation Data Streaming**. We do not use C libraries to manually construct JSON responses. Instead, SQL Server generates the JSON natively (e.g., using `FOR JSON PATH`), and the C server acts as a highly concurrent proxy, streaming the raw bytes directly from the database driver into the `libevent` network socket buffer.

---

## Step 1: Define the Input Schema

To securely accept JSON input, define a strict schema using `FieldValidator` arrays. This guarantees that malformed requests are rejected with a `400 Bad Request` before they ever reach your handler logic.

```c
// 1. Define the expected fields and their types
static const FieldValidator CustomerSchema[] = {
    {.field_name = "customer_id", .type = TYPE_INT, .is_required = true, .custom_validator = nullptr}
};

// 2. Wrap it in a ValidationContext
const ValidationContext CustomerContext = {
    .schema = CustomerSchema,
    .schema_count = sizeof(CustomerSchema) / sizeof(CustomerSchema[0]),
    .global_validator = nullptr // Optional: Use for cross-field logic (e.g., start_date < end_date)
};
```

*Note: For complex business invariants involving multiple fields, implement a `global_validator` function (see `sales_invariant_validator` in `handlers.c` as a model).*

---

## Step 2: Write the ODBC Parameter Binder

Create a lightweight callback function that maps the validated JSON payload into your SQL Server Stored Procedure parameters.

```c
static void customer_bind_cb(struct json_object* body, SQLHSTMT hstmt) {
    // 1. Safely extract the pre-validated value
    int customer_id = json_get_int(json_object_object_get(body, "customer_id"));
    
    // 2. Bind it to the prepared statement
    SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &customer_id, 0, nullptr);
}
```

---

## Step 3: Implement the Handler Function

The handler function orchestrates the request. By using the `odbcutil_get_json` abstraction, you eliminate all boilerplate connection handling, fetching, and memory allocation.

```c
void customer_get_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    // Set default success headers
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    // Execute the stored procedure and stream the results DIRECTLY to the network buffer.
    // The macro __func__ injects the caller name for robust telemetry.
    if (!odbcutil_get_json("{CALL sp_customer_get(?)}", customer_bind_cb, body, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }
}
```

### ⚡ Why is this "Well-Behaved"?
* **Zero Allocations:** `odbcutil_get_json` reads the `FOR JSON` chunks from SQL Server and pipes them instantly into `out_buf` (`evbuffer_add`). No intermediate `json-c` objects are allocated or freed.
* **Thread-Local DB Multiplexing:** `odbcutil_connect` inherently utilizes `_Thread_local` connection pooling. There are **zero mutex locks** acquired during the execution path, allowing horizontal scaling across all CPU cores.

---

## Step 4: Register the Route

Finally, wire the handler into the `libevent` routing table inside `server.c`. You map the URI to your handler and validation schema, and specify security context flags.

```c
// Inside server.c (e.g., in a route configuration function)
{ 
    .path = "/customer/get", 
    .allowed_method = EVHTTP_REQ_POST, 
    .validation_ctx = &CustomerContext, 
    .handler = customer_get_handler, 
    .user_arg = nullptr, 
    .is_fast = true,   // True if the query is extremely fast (< 5ms)
    .is_secure = true  // True to enforce Bearer JWT Authentication
}
```

---

## Fast-Track: Simple Handlers (No Input Parameters)

If your endpoint doesn't accept a JSON payload and simply retrieves global data (like the `/shippers` endpoint), the implementation is even more concise. 

You do **not** need a schema, `ValidationContext`, or parameter binder callback.

### The Handler
Pass `nullptr` for the binder and body in `odbcutil_get_json`:

```c
void shippers_handler(
    [[maybe_unused]] struct evhttp_request* req, 
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
        
    // Execute parameterless query and stream directly to client
    if (!odbcutil_get_json("{CALL sp_shippers_view}", nullptr, nullptr, out_buf, __func__)) {
        *out_status = HTTP_INTERNAL;
        *out_status_txt = "Internal Server Error";
    }
}
```

### The Route Registration
In `server.c`, set `.allowed_method = EVHTTP_REQ_GET` and `.validation_ctx = nullptr`:

```c
{ 
    .path = "/shippers", 
    .allowed_method = EVHTTP_REQ_GET, 
    .validation_ctx = nullptr, 
    .handler = shippers_handler, 
    .user_arg = nullptr, 
    .is_fast = true, 
    .is_secure = true 
}
```

---

## Architectural Highlights to Remember
1. **Never use `json-c` for API Responses:** Building dynamic JSON responses using `json_object_new_*` taxes the heap memory severely. Rely on the database (`FOR JSON`) and `evbuffer` instead.
2. **Lock-Free by Design:** The hot-path architecture has been deliberately designed to avoid Read/Write locks. Do not introduce global state mutexes in your handlers.
3. **Implicit Cleanup:** Connections are tied to the worker thread's lifetime, and the HTTP request lifecycle is managed by `libevent`. Your handler simply returns, and all state (including the parsed input body) is automatically freed by the router.
