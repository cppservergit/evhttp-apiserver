import re
with open("src/handlers.c", "r") as f:
    s = f.read()
s = s.replace("[[maybe_unused]] \n    struct evbuffer", "struct evbuffer")
s = s.replace("[[maybe_unused]] \n", "")
s = s.replace("int* out_status, \n    \n    struct evbuffer", "int* out_status, \n    struct evbuffer")
with open("src/handlers.c", "w") as f:
    f.write(s)
