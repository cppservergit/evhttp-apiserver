import re
with open("src/http_client.c", "r") as f:
    s = f.read()

s = s.replace("chunk->memory = malloc(1);", "chunk->memory = malloc(4096);")
s = s.replace("return false;", "return nullptr;")
s = s.replace('LOG_ERROR("HTTP %ld from %s | Response: %s", http_code, url, chunk->memory ? chunk->memory : "<empty>");', 
              'set_thread_error(TL_ERR_ERROR, "HTTP %ld from %s | Response: %s", http_code, url, chunk->memory ? chunk->memory : "<empty>");')

with open("src/http_client.c", "w") as f:
    f.write(s)
