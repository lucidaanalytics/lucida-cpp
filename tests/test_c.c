/* C API smoke test: compiled as C, linked with src/lucida_c.cpp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lucida/lucida.h"

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

static char last_url[256];
static char last_body[8192];
static int posts = 0;
static void *pending = NULL;

static void post(void *user, const char *url, const char *body, void *request) {
    (void)user;
    posts++;
    snprintf(last_url, sizeof last_url, "%s", url);
    snprintf(last_body, sizeof last_body, "%s", body);
    pending = request; /* answered later, like a real async HTTP stack */
}

static int sets = 0;
static char *get(void *user, const char *key) { (void)user; (void)key; return NULL; }
static void set(void *user, const char *key, const char *value) { (void)user; (void)key; (void)value; sets++; }
static void rm(void *user, const char *key) { (void)user; (void)key; }

int main(void) {
    lucida_options o;
    memset(&o, 0, sizeof o);
    CHECK(lucida_new(&o) == NULL);
    o.api_key = "lc_pk_c";
    o.host = "https://example.test";
    o.opt_in = 1;
    o.post = post;
    o.get = get;
    o.set = set;
    o.remove = rm;
    lucida_client *c = lucida_new(&o);
    CHECK(c != NULL);
    const char *keys[] = {"level", "$lc_org"};
    const char *values[] = {"3", "spoof"};
    CHECK(lucida_capture(c, "early", keys, values, 2) == 0);
    CHECK(sets == 0); /* no storage before consent */
    lucida_opt_in(c);
    CHECK(sets > 0);
    CHECK(lucida_capture(c, "level_complete", keys, values, 2) == 1);
    CHECK(lucida_identify(c, "user-1") == 1);
    CHECK(lucida_queued(c) == 3);
    lucida_flush(c);
    CHECK(posts == 1 && strcmp(last_url, "https://example.test/v1/batch") == 0);
    CHECK(strstr(last_body, "\"level\":\"3\"") != NULL);
    CHECK(strstr(last_body, "$lc_org") == NULL);
    CHECK(strstr(last_body, "\"$lib\":\"lucida-c\"") != NULL);
    lucida_request_done(pending, 200);
    CHECK(lucida_queued(c) == 0);
    lucida_free(c);
    printf("lucida c tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
