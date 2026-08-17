#include <apiserver/context.h>
#include <stdio.h>

static _Thread_local char tl_user[33] = {0};
static _Thread_local char tl_session[37] = {0};
static _Thread_local char tl_client_ip[64] = {0};
static _Thread_local char tl_uri[1024] = {0};
static _Thread_local char tl_content_type[128] = {0};

void context_set(const char* user, const char* session, const char* client_ip, const char* uri) {
    if (user) (void)snprintf(tl_user, sizeof(tl_user), "%s", user);
    else tl_user[0] = '\0';
    if (session) (void)snprintf(tl_session, sizeof(tl_session), "%s", session);
    else tl_session[0] = '\0';
    if (client_ip) (void)snprintf(tl_client_ip, sizeof(tl_client_ip), "%s", client_ip);
    else tl_client_ip[0] = '\0';
    if (uri) (void)snprintf(tl_uri, sizeof(tl_uri), "%s", uri);
    else tl_uri[0] = '\0';
}

void context_clear(void) {
    tl_user[0] = '\0';
    tl_session[0] = '\0';
    tl_client_ip[0] = '\0';
    tl_uri[0] = '\0';
    tl_content_type[0] = '\0';
}

const char* context_get_user(void) {
    return tl_user[0] ? tl_user : NULL;
}

const char* context_get_session_id(void) {
    return tl_session[0] ? tl_session : NULL;
}

const char* context_get_client_ip(void) {
    return tl_client_ip[0] ? tl_client_ip : NULL;
}

void context_set_content_type(const char* ctype) {
    if (ctype) (void)snprintf(tl_content_type, sizeof(tl_content_type), "%s", ctype);
    else tl_content_type[0] = '\0';
}

const char* context_get_content_type(void) {
    return tl_content_type[0] ? tl_content_type : NULL;
}
