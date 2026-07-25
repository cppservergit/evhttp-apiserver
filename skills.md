# Guide: Writing a Well-Behaved Database API Handler in EvHttp

This document is a step-by-step guide for C programmers implementing secure, ultra-high-performance API endpoints in `evhttp-apiserver`. 

The core philosophy of this server relies on **Zero-Allocation Data Streaming**. We do not use C libraries to manually construct JSON responses. Instead, SQL Server generates the JSON natively (e.g., using `FOR JSON PATH`), and the C server acts as a highly concurrent proxy, streaming the raw bytes directly from the database driver into the `libevent` network socket buffer.

---

## Step 1: Define the Input Schema

To securely accept JSON input, define a strict schema using `FieldValidator` arrays. This guarantees that malformed requests are rejected with a `400 Bad Request` before they ever reach your handler logic.

```c
// 1. Define custom validators for complex business logic (Optional)
static bool customer_id_validator(
    [[maybe_unused]] const ValidationContext *ctx, 
    const json_object *obj, 
    [[maybe_unused]] const char *name, 
    char *err_buf, 
    size_t err_len
) {
    const char *id = json_object_get_string((json_object *)obj);
    if (!id || strlen(id) != 5) {
        return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id ? id : "null");
    }
    for (int i = 0; i < 5; ++i) {
        if (!isalpha((unsigned char)id[i])) {
            return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id);
        }
    }
    return true;
}

// 2. Define the expected fields and their types
static const FieldValidator CustomerSchema[] = {
    // We use a custom validator to enforce the ID is exactly 5 alphabetical chars
    {.field_name = "id", .type = TYPE_STRING, .is_required = true, .custom_validator = customer_id_validator}
};

// 3. Wrap it in a ValidationContext
const ValidationContext CustomerContext = {
    .schema = CustomerSchema,
    .schema_count = sizeof(CustomerSchema) / sizeof(CustomerSchema[0]),
    .global_validator = nullptr // Optional: Use for cross-field logic (e.g., start_date < end_date)
};
```

*Note: For complex business invariants involving multiple fields, implement a `global_validator` function (see `sales_invariant_validator` in `handlers.c` as a model).*

---

## Step 2: Implement the Handler Function

The handler function orchestrates the request. By using the `odbcutil_get_json` abstraction and a `QueryParam` array, you eliminate all boilerplate connection handling, fetching, and memory allocation.

```c
void customer_handler(
    struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    // Set default success headers
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
    
    // 1. Safely extract the pre-validated string
    const char* customer_id = json_get_string(body, "id");
    
    // 2. Map the payload into your SQL Server Stored Procedure parameters
    QueryParam params[] = {
        { .type = PARAM_STRING, .value = customer_id }
    };
    
    // 3. Execute the stored procedure and stream the results DIRECTLY to the network buffer.
    // The macro __func__ injects the caller name for robust telemetry.
    if (!odbcutil_get_json("{CALL sp_customer_get(?)}", params, ARRAY_SIZE(params), out_buf, __func__)) {
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
    .path = "/customer", 
    .allowed_method = EVHTTP_REQ_POST, 
    .validation_ctx = &CustomerContext, 
    .handler = customer_handler, 
    .user_arg = nullptr, 
    .is_fast = true,   // True if the query is extremely fast (< 5ms)
    .auth_mode = AUTH_JWT  // Require Bearer JWT Authentication
}
```

---

## Fast-Track: Simple Handlers (No Input Parameters)

If your endpoint doesn't accept a JSON payload and simply retrieves global data (like the `/shippers` endpoint), the implementation is even more concise. 

You do **not** need a schema, `ValidationContext`, or parameter binder callback.

### The Handler
Pass `nullptr` and `0` for the parameter arguments in `odbcutil_get_json`:

```c
void shippers_handler(
    [[maybe_unused]] struct json_object* body, 
    [[maybe_unused]] void* arg, 
    int* out_status, 
    const char** out_status_txt,
    struct evbuffer* out_buf
) {
    *out_status = HTTP_OK;
    *out_status_txt = "OK";
        
    // Execute parameterless query and stream directly to client
    if (!odbcutil_get_json("{CALL sp_shippers_view}", nullptr, 0, out_buf, __func__)) {
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
    .auth_mode = AUTH_JWT 
}
```

---

## Architectural Highlights to Remember
1. **Never use `json-c` for API Responses:** Building dynamic JSON responses using `json_object_new_*` taxes the heap memory severely. Rely on the database (`FOR JSON`) and `evbuffer` instead.
2. **Lock-Free by Design:** The hot-path architecture has been deliberately designed to avoid Read/Write locks. Do not introduce global state mutexes in your handlers.
3. **Implicit Cleanup:** Connections are tied to the worker thread's lifetime, and the HTTP request lifecycle is managed by `libevent`. Your handler simply returns, and all state (including the parsed input body) is automatically freed by the router.
4. **100% SQL Injection Protection:** By exclusively using ODBC Prepared Statements (`SQLBindParameter`) to map JSON inputs to Stored Procedure variables, the API is natively immune to all SQL Injection attacks. Input values are never concatenated into the query string.
