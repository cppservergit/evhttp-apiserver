import glob

files = glob.glob("src/*.c")
for f in files:
    with open(f, 'r') as file:
        c = file.read()
    
    # Replace odbcutil_get_rs2json(
    c = c.replace('odbcutil_get_rs2json(', 'odbcutil_get_rs2json(DB_0, ')
    
    # Replace odbcutil_get_json(
    c = c.replace('odbcutil_get_json(', 'odbcutil_get_json(DB_0, ')
    
    # Clean up double DB_0 if we accidentally patched something twice
    c = c.replace('odbcutil_get_rs2json(DB_0, DB_0, ', 'odbcutil_get_rs2json(DB_0, ')
    c = c.replace('odbcutil_get_json(DB_0, DB_0, ', 'odbcutil_get_json(DB_0, ')
    
    with open(f, 'w') as file:
        file.write(c)

