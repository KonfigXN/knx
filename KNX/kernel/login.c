#include "knx.h"

static int login_ready;
static int login_authenticated;
static const char *login_name;

static int same_text(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

int knx_login_init(void) {
    login_ready = 1;
    login_authenticated = 0;
    login_name = 0;
    knx_log("[USER]", "Local console login ready (use `login root`)");
    return 1;
}

int knx_login_attempt(const char *username) {
    if (!login_ready || !username || !same_text(username, "root"))
        return 0;
    login_authenticated = 1;
    login_name = "root";
    return 1;
}

int knx_login_is_authenticated(void) { return login_ready && login_authenticated; }

const char *knx_login_user(void) { return login_authenticated ? login_name : ""; }
