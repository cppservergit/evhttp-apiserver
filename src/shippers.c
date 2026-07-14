#include <sql.h>
#include <sqlext.h>
#include <stdlib.h>
#include "odbcutil.h"
#include <json-c/json.h>
#include "shippers.h"
#include "server.h"

struct json_object* shippers_get_data(void) {
    return odbcutil_get_json("{CALL sp_shippers_view}", __func__);
}
