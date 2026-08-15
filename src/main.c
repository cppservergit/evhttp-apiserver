#include "server.h"
#include "handlers.h"

static const middleware_ctx_t g_routes[] = {
    { .path = "/ping", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &ping_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/version", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &version_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/sysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &sysinfo_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/rsysinfo", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &rsysinfo_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/rcustomer", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .handler = &rcustomer_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/customer", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomerContext, .handler = &customer_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/sales", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SalesContext, .handler = &sales_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/shippers", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &shippers_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/products", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &products_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/uuid", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &uuid_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/secretb32", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &secretb32_handler, .is_fast = true, .auth_mode = AUTH_NONE },
    { .path = "/login", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &LoginContext, .handler = &login_handler, .is_fast = false, .auth_mode = AUTH_NONE },
    { .path = "/getqr", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &getqr_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/verifytotp", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &VerifyTotpContext, .handler = &verifytotp_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/metrics", .allowed_method = EVHTTP_REQ_GET, .validation_ctx = nullptr, .handler = &metrics_handler, .is_fast = true, .auth_mode = AUTH_API_KEY },
    { .path = "/employee", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &EmployeeContext, .handler = &employee_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/prodget", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &ProdgetContext, .handler = &prodget_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/supplier", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SupplierContext, .handler = &supplier_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/customers", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &CustomersContext, .handler = &customers_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/salespgsql", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &SalesContext, .handler = &sales_pgsql_handler, .is_fast = true, .auth_mode = AUTH_JWT },
    { .path = "/upload", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = &UploadContext, .handler = &upload_handler, .is_fast = false, .auth_mode = AUTH_JWT },
    { .path = "/mcp", .allowed_method = EVHTTP_REQ_POST, .validation_ctx = nullptr, .handler = &mcp_handler, .is_fast = true, .auth_mode = AUTH_NONE }
};
static const size_t g_route_count = sizeof(g_routes) / sizeof(g_routes[0]);

int main(void) {
    server_register_routes(g_routes, g_route_count);
    return server_start();
}
