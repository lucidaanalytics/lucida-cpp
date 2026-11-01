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

typedef struct lucida_options {
    const char *api_key;
    const char *host;   /* NULL = https://app.lucida.dev */
    int opt_in;         /* 1: nothing is captured or stored before lucida_opt_in() */
    lucida_post_fn post;
    /* Optional persistence (all three or none; NULL = memory only). `get` returns a malloc'd string
     * (freed by the SDK with free()) or NULL when missing. Never called before analytics consent. */
    char *(*get)(void *user, const char *key);
    void (*set)(void *user, const char *key, const char *value);
    void (*remove)(void *user, const char *key);
    void *user;
} lucida_options;

/* NULL when api_key or post is missing. */
lucida_client *lucida_new(const lucida_options *options);
/* Free only after every pending request was completed with lucida_request_done. */
void lucida_free(lucida_client *client);

/* String properties as parallel key/value arrays (n may be 0). Returns 1 when queued, 0 when consent blocks it. */
int lucida_capture(lucida_client *client, const char *event, const char *const *keys, const char *const *values, size_t n);
int lucida_screen(lucida_client *client, const char *name);
int lucida_identify(lucida_client *client, const char *user_id);
void lucida_reset(lucida_client *client);
void lucida_opt_in(lucida_client *client);
void lucida_opt_out(lucida_client *client);
void lucida_tick(lucida_client *client);
void lucida_flush(lucida_client *client);
size_t lucida_queued(const lucida_client *client);

void lucida_request_done(void *request, int status);

#ifdef __cplusplus
}
#endif

#endif /* LUCIDA_H */
