// C API over lucida::Client (include/lucida/lucida.h).
#include "lucida/lucida.h"

#include <cstdlib>
#include <memory>

#include "lucida/lucida.hpp"

namespace {

class CTransport : public lucida::Transport {
public:
    CTransport(lucida_post_fn post, void* user) : post_(post), user_(user) {}
    void post(const std::string& url, const std::string& body, std::function<void(int)> done) override {
        // The handle owns the callback until lucida_request_done runs it.
        post_(user_, url.c_str(), body.c_str(), new std::function<void(int)>(std::move(done)));
    }

private:
    lucida_post_fn post_;
    void* user_;
};

class CStorage : public lucida::Storage {
public:
    explicit CStorage(const lucida_options& o) : o_(o) {}
    std::string get(const std::string& key) override {
        if (!o_.get) return mem_.get(key);
        char* v = o_.get(o_.user, key.c_str());
        std::string s = v ? v : "";
        std::free(v);
        return s;
    }
    void set(const std::string& key, const std::string& value) override {
        if (o_.set) o_.set(o_.user, key.c_str(), value.c_str());
        else mem_.set(key, value);
    }
    void remove(const std::string& key) override {
        if (o_.remove) o_.remove(o_.user, key.c_str());
        else mem_.remove(key);
    }

private:
    lucida_options o_;
    lucida::MemoryStorage mem_;
};

} // namespace

struct lucida_client {
    CTransport transport;
    CStorage storage;
    lucida::Client client;

    lucida_client(const lucida_options& o, lucida::Config cfg)
        : transport(o.post, o.user), storage(o), client(std::move(cfg), transport, storage) {}
};

extern "C" {

lucida_client* lucida_new(const lucida_options* o) {
    if (!o || !o->api_key || !*o->api_key || !o->post) return nullptr;
    lucida::Config cfg;
    cfg.api_key = o->api_key;
    if (o->host) cfg.host = o->host;
    cfg.consent = o->opt_in ? lucida::ConsentMode::OptIn : lucida::ConsentMode::OptOut;
    cfg.lib = "lucida-c";
    return new lucida_client(*o, std::move(cfg));
}

void lucida_free(lucida_client* c) { delete c; }

int lucida_capture(lucida_client* c, const char* event, const char* const* keys, const char* const* values, size_t n) {
    if (!c || !event) return 0;
    lucida::Props p;
    for (size_t i = 0; i < n; i++)
        if (keys[i] && values[i]) p[keys[i]] = std::string(values[i]);
    return c->client.capture(event, p) ? 1 : 0;
}

int lucida_screen(lucida_client* c, const char* name) { return c && name && c->client.screen(name) ? 1 : 0; }
int lucida_identify(lucida_client* c, const char* id) { return c && id && c->client.identify(id) ? 1 : 0; }
void lucida_reset(lucida_client* c) { if (c) c->client.reset(); }
void lucida_opt_in(lucida_client* c) { if (c) c->client.opt_in(); }
void lucida_opt_out(lucida_client* c) { if (c) c->client.opt_out(); }
void lucida_tick(lucida_client* c) { if (c) c->client.tick(); }
void lucida_flush(lucida_client* c) { if (c) c->client.flush(); }
size_t lucida_queued(const lucida_client* c) { return c ? c->client.queued() : 0; }

void lucida_request_done(void* request, int status) {
    std::unique_ptr<std::function<void(int)>> done(static_cast<std::function<void(int)>*>(request));
    if (done && *done) (*done)(status);
}

} // extern "C"
