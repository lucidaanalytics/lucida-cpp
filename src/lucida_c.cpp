// C API over lucida::Client (include/lucida/lucida.h).
#include "lucida/lucida.h"

#include <cstdlib>
#include <memory>

#include "lucida/flags.hpp"
#include "lucida/lucida.hpp"

namespace {

class CTransport : public lucida::Transport {
public:
    CTransport(lucida_post_fn post, lucida_request_fn request, void* user) : post_(post), request_(request), user_(user) {}
    void post(const std::string& url, const std::string& body, std::function<void(int)> done) override {
        // The handle owns the callback until lucida_request_done runs it.
        post_(user_, url.c_str(), body.c_str(), new std::function<void(int)>(std::move(done)));
    }

    void request(const std::string& url, const std::string& body, std::function<void(int, const std::string&)> done) override {
        if (!request_) {
            done(0, std::string());
            return;
        }
        request_(user_, url.c_str(), body.c_str(), new std::function<void(int, const std::string&)>(std::move(done)));
    }

private:
    lucida_post_fn post_;
    lucida_request_fn request_;
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

lucida::Props to_props(const char* const* keys, const char* const* values, size_t n) {
    lucida::Props p;
    for (size_t i = 0; i < n; i++)
        if (keys[i] && values[i]) p[keys[i]] = std::string(values[i]);
    return p;
}

} // namespace

struct lucida_client {
    CTransport transport;
    CStorage storage;
    lucida::Client client;
    lucida::Flags flags;

    lucida_client(const lucida_options& o, lucida::Config cfg)
        : transport(o.post, o.request, o.user), storage(o), client(std::move(cfg), transport, storage), flags(client) {}
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
    return c->client.capture(event, to_props(keys, values, n)) ? 1 : 0;
}

int lucida_alias(lucida_client* c, const char* alias, const char* distinct_id) {
    return c && alias && c->client.alias(alias, distinct_id ? distinct_id : "") ? 1 : 0;
}

int lucida_group_identify(lucida_client* c, const char* type, const char* key, const char* const* keys, const char* const* values,
                          size_t n, const char* owner_id) {
    if (!c || !type || !key) return 0;
    return c->client.group_identify(type, key, to_props(keys, values, n), owner_id ? owner_id : "") ? 1 : 0;
}

int lucida_evaluate_flags(lucida_client* c, const char* flag_key, const char* const* pkeys, const char* const* pvalues, size_t n,
                          lucida_flags_fn cb, void* user) {
    if (!c || !cb) return 0;
    lucida::FlagOptions o;
    o.person_properties = to_props(pkeys, pvalues, n);
    auto call = [cb, user](bool ok, const std::vector<lucida::FlagResult>& flags) {
        std::vector<std::string> payloads, variants;
        for (const auto& f : flags) {
            std::string json;
            lucida::detail::json_value(json, f.payload);
            payloads.push_back(std::move(json));
            variants.push_back(f.variant());
        }
        std::vector<lucida_flag> out;
        for (size_t i = 0; i < flags.size(); i++) {
            const lucida::FlagResult& f = flags[i];
            const bool has_payload = !std::holds_alternative<std::nullptr_t>(f.payload.v);
            out.push_back({f.key.c_str(), f.enabled() ? 1 : 0, variants[i].empty() ? nullptr : variants[i].c_str(),
                           has_payload ? payloads[i].c_str() : nullptr, f.reason.c_str(), f.condition_index, f.request_id.c_str()});
        }
        cb(user, ok ? 1 : 0, out.data(), out.size());
    };
    if (flag_key) {
        c->flags.get(flag_key, o, [call](bool found, const lucida::FlagResult& f) {
            std::vector<lucida::FlagResult> one;
            if (found) one.push_back(f);
            call(true, one);
        });
    } else {
        c->flags.evaluate_all(o, call);
    }
    return 1;
}

int lucida_screen(lucida_client* c, const char* name) { return c && name && c->client.screen(name) ? 1 : 0; }
int lucida_identify(lucida_client* c, const char* id) { return c && id && c->client.identify(id) ? 1 : 0; }
void lucida_reset(lucida_client* c) { if (c) c->client.reset(); }
void lucida_opt_in(lucida_client* c) { if (c) c->client.opt_in(); }
void lucida_opt_out(lucida_client* c) { if (c) c->client.opt_out(); }
void lucida_tick(lucida_client* c) { if (c) c->client.tick(); }
void lucida_flush(lucida_client* c) { if (c) c->client.flush(); }
size_t lucida_queued(const lucida_client* c) { return c ? c->client.queued() : 0; }

void lucida_response_done(void* request, int status, const char* body) {
    using Done = std::function<void(int, const std::string&)>;
    std::unique_ptr<Done> done(static_cast<Done*>(request));
    if (done && *done) (*done)(status, body ? std::string(body) : std::string());
}

void lucida_request_done(void* request, int status) {
    std::unique_ptr<std::function<void(int)>> done(static_cast<std::function<void(int)>*>(request));
    if (done && *done) (*done)(status);
}

} // extern "C"
