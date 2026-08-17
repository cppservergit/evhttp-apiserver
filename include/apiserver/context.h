#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void context_set(const char* user, const char* session, const char* client_ip, const char* uri);
void context_clear(void);
const char* context_get_user(void);
const char* context_get_session_id(void);
const char* context_get_client_ip(void);
void context_set_content_type(const char* ctype);
const char* context_get_content_type(void);

#ifdef __cplusplus
}
#endif
