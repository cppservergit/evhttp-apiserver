#include <sql.h>
#include <sqlext.h>
#include <stdlib.h>
#include "odbcutil.h"
#include <json-c/json.h>
#include "products.h"
#include "server.h"

struct json_object* products_get_data(void) {
    return odbcutil_get_json("{CALL sp_products_view}", __func__);
}
