import re

with open('src/totp.c', 'r') as f:
    c = f.read()

c = c.replace('odbcutil_connect()', 'odbcutil_connect(DB_0)')
c = c.replace('odbcutil_alloc_stmt(hdbc', 'odbcutil_alloc_stmt(DB_0, hdbc')
c = c.replace('odbcutil_set_error(SQL_HANDLE_STMT', 'odbcutil_set_error(DB_0, SQL_HANDLE_STMT')
c = c.replace('odbcutil_set_error(SQL_HANDLE_DBC', 'odbcutil_set_error(DB_0, SQL_HANDLE_DBC')
c = c.replace('odbcutil_set_error(SQL_HANDLE_ENV', 'odbcutil_set_error(DB_0, SQL_HANDLE_ENV')

with open('src/totp.c', 'w') as f:
    f.write(c)

