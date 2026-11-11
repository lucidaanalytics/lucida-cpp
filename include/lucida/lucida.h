/* Lucida C API: a thin wrapper over the C++ client (compile src/lucida_c.cpp into your build).
 * The host supplies the HTTP transport (and optionally persistence) as callbacks. */
#ifndef LUCIDA_H
#define LUCIDA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lucida_client lucida_client;

/* Send `body` as a JSON POST to `url`, then call lucida_request_done(request, status) exactly once,
 * from any thread (status 0 = network failure). */
typedef void (*lucida_post_fn)(void *user, const char *url, const char *body, void *request);

/* Like lucida_post_fn, but the response body is needed (flag evaluation): call lucida_response_done(request, status, body)
 * exactly once, from any thread (status 0 = network failure, body may be NULL). */
typedef void (*lucida_request_fn)(void *user, const char *url, const char *body, void *request);

typedef struct lucida_options {
    const char *api_key;
    const char *host;   /* NULL = https://capture.lucidace.com */
    int opt_in;         /* 1: nothing is captured or stored before lucida_opt_in() */
    lucida_post_fn post;
    /* Optional persistence (all three or none; NULL = memory only). `get` returns a malloc'd string
     * (freed by the SDK with free()) or NULL when missing. Never called before analytics consent. */
    char *(*get)(void *user, const char *key);
    void (*set)(void *user, const char *key, const char *value);
    void (*remove)(void *user, const char *key);
    void *user;
    /* Optional: needed for lucida_evaluate_flags(); without it flag evaluation reports failure. Add new fields only at the end. */
    lucida_request_fn request;
} lucida_options;

/* One decided flag. All strings are valid only during the callback. `variant` and `payload_json` are NULL when absent. */
typedef struct lucida_flag {
    const char *key;
    int enabled;               /* 1 unless the value is false */
    const char *variant;       /* variant key of a multivariate flag */
    const char *payload_json;  /* the payload as JSON text */
    const char *reason;
    int condition_index;       /* -1 when none */
    const char *request_id;
} lucida_flag;

/* Called exactly once per lucida_evaluate_flags. Evaluating every flag: ok = 0 means no consent, no request transport, a
 * failed call or a bad answer (n is 0). With a flag key: n is 1 when the gateway decided it, else 0 (unknown or failed). */
typedef void (*lucida_flags_fn)(void *user, int ok, const lucida_flag *flags, size_t n);

/* NULL when api_key or post is missing. */
lucida_client *lucida_new(const lucida_options *options);
/* Free only after every pending request was completed with lucida_request_done. */
void lucida_free(lucida_client *client);

/* String properties as parallel key/value arrays (n may be 0). Returns 1 when queued, 0 when consent blocks it. */
int lucida_capture(lucida_client *client, const char *event, const char *const *keys, const char *const *values, size_t n);
int lucida_screen(lucida_client *client, const char *name);
int lucida_identify(lucida_client *client, const char *user_id);
/* Links alias to distinct_id (NULL = the client's current id). */
int lucida_alias(lucida_client *client, const char *alias, const char *distinct_id);
/* Group properties as parallel key/value arrays; owner_id NULL = the group itself ("$<type>_<key>"). */
int lucida_group_identify(lucida_client *client, const char *group_type, const char *group_key, const char *const *keys,
                          const char *const *values, size_t n, const char *owner_id);

/* Remote flag evaluation (POST /v1/flags). flag_key NULL evaluates every flag; with a key the exposure is recorded.
 * Person properties are string key/value arrays (n may be 0). Returns 0 (callback not called) for NULL client or callback. */
int lucida_evaluate_flags(lucida_client *client, const char *flag_key, const char *const *person_keys,
                          const char *const *person_values, size_t n, lucida_flags_fn callback, void *user);
void lucida_reset(lucida_client *client);
void lucida_opt_in(lucida_client *client);
void lucida_opt_out(lucida_client *client);
void lucida_tick(lucida_client *client);
void lucida_flush(lucida_client *client);
size_t lucida_queued(const lucida_client *client);

void lucida_request_done(void *request, int status);
void lucida_response_done(void *request, int status, const char *body);

#ifdef __cplusplus
}
#endif

#endif /* LUCIDA_H */
