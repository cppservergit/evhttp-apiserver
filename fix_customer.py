import re
with open("src/customer.c", "r") as f:
    s = f.read()

s = s.replace('LOG_ERROR("Login payload truncated");', 'set_thread_error(TL_ERR_ERROR, "Login payload truncated");')
s = s.replace('LOG_WARN("[customer] JWT cache miss (token missing or expired). Requesting a fresh token...");', 
              'set_thread_error(TL_ERR_WARN, "[customer] JWT cache miss (token missing or expired). Requesting a fresh token...");')
s = s.replace('LOG_WARN("Login backend returned %ld", http_code);', 
              'set_thread_error(TL_ERR_WARN, "Login backend returned %ld", http_code);')
s = s.replace('LOG_WARN("Failed to extract access_token from login response");', 
              'set_thread_error(TL_ERR_WARN, "Failed to extract access_token from login response");')

with open("src/customer.c", "w") as f:
    f.write(s)
