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

static char flag_body[2048];
static void *flag_pending = NULL;
static void flag_request(void *user, const char *url, const char *body, void *request) {
    (void)user;
    CHECK(strcmp(url, "https://example.test/v1/flags") == 0);
    snprintf(flag_body, sizeof flag_body, "%s", body);
    flag_pending = request;
}

static int flag_calls = 0, flag_ok = -1;
static size_t flag_n = 0;
static char flag_seen[64], flag_variant[16], flag_payload[64];
static void on_flags(void *user, int ok, const lucida_flag *flags, size_t n) {
    (void)user;
    flag_calls++;
    flag_ok = ok;
    flag_n = n;
    if (n > 0) {
        snprintf(flag_seen, sizeof flag_seen, "%s", flags[0].key);
        snprintf(flag_variant, sizeof flag_variant, "%s", flags[0].variant ? flags[0].variant : "");
        snprintf(flag_payload, sizeof flag_payload, "%s", flags[0].payload_json ? flags[0].payload_json : "");
    }
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
    /* alias and group identify */
    const char *gk[] = {"plan"};
    const char *gv[] = {"pro"};
    CHECK(lucida_alias(c, "old-name", NULL) == 1);
    CHECK(lucida_group_identify(c, "org", "acme", gk, gv, 1, NULL) == 1);
    CHECK(lucida_group_identify(c, NULL, "acme", gk, gv, 1, NULL) == 0);
    lucida_flush(c);
    CHECK(strstr(last_body, "\"$create_alias\"") != NULL && strstr(last_body, "\"alias\":\"old-name\"") != NULL);
    CHECK(strstr(last_body, "\"$groupidentify\"") != NULL && strstr(last_body, "\"$group_set\":{\"plan\":\"pro\"}") != NULL);
    lucida_request_done(pending, 200);

    /* remote flags: without a request transport the call reports failure, with one the body is parsed */
    CHECK(lucida_evaluate_flags(NULL, NULL, NULL, NULL, 0, on_flags, NULL) == 0);
    CHECK(lucida_evaluate_flags(c, "checkout", NULL, NULL, 0, on_flags, NULL) == 1);
    CHECK(flag_calls == 1 && flag_n == 0); /* o.request is NULL */
    lucida_free(c);
    o.request = flag_request;
    c = lucida_new(&o);
    lucida_opt_in(c);
    const char *pk[] = {"plan"};
    const char *pv[] = {"pro"};
    CHECK(lucida_evaluate_flags(c, "checkout", pk, pv, 1, on_flags, NULL) == 1);
    CHECK(strstr(flag_body, "\"flag_keys\":[\"checkout\"]") != NULL && strstr(flag_body, "\"person_properties\":{\"plan\":\"pro\"}") != NULL);
    CHECK(flag_calls == 1); /* answered later */
    lucida_response_done(flag_pending,
                         200, "{\"flags\":{\"checkout\":{\"enabled\":true,\"variant\":\"b\",\"payload\":{\"n\":1}}},\"request_id\":\"r\"}");
    CHECK(flag_calls == 2 && flag_ok == 1 && flag_n == 1);
    CHECK(strcmp(flag_seen, "checkout") == 0 && strcmp(flag_variant, "b") == 0 && strcmp(flag_payload, "{\"n\":1}") == 0);
    lucida_free(c);
    printf("lucida c tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
