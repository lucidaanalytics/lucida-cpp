// Lucida C++ SDK: remote feature-flag evaluation (POST <host>/v1/flags), the same request and response as the other SDKs.
// Needs a Transport that implements request() (the response body); with the default one every call reports failure.
// Local evaluation (definitions + evaluator) is not part of this SDK: flags are decided by the gateway.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lucida/json.hpp"
#include "lucida/lucida.hpp"

namespace lucida {

/// Inputs of one evaluation. The distinct id (and the anon id after identify) come from the client.
struct FlagOptions {
    Props person_properties;
    std::map<std::string, std::string> groups;  // group type -> group key
    Props group_properties;                      // group type -> Object of properties
    std::vector<std::string> app_contexts;
    std::string app_version;
    std::string bucketing_key;
    bool send_exposure = true; // get(): emit `$feature_flag_called` + `$exposure` once per (id, flag, value)
};

/// A decided flag. `value` is a bool or the variant key (string); `payload` is any JSON (null when none).
struct FlagResult {
    std::string key;
    Value value;
    Value payload;
    std::string reason;
    int condition_index = -1;
    std::string request_id;

    bool enabled() const {
        const bool* b = std::get_if<bool>(&value.v);
        return !(b && !*b) && !std::holds_alternative<std::nullptr_t>(value.v);
    }
    /// The variant key, or "" for a boolean flag.
    std::string variant() const {
        const std::string* s = std::get_if<std::string>(&value.v);
        return s ? *s : std::string();
    }
};

/// Remote flag reads for one Client. Keep it (and the Client) alive until its callbacks ran.
class Flags {
public:
    using AllCallback = std::function<void(bool ok, const std::vector<FlagResult>& flags)>;
    using OneCallback = std::function<void(bool found, const FlagResult& flag)>;

    explicit Flags(Client& client) : client_(client) {}

    /// Evaluates every flag (or only `keys`). `done(false, {})` without consent, a transport that has no request(),
    /// a non-2xx answer, or a body that is not a flags response.
    void evaluate(const FlagOptions& o, const std::vector<std::string>& keys, AllCallback done) {
        if (!client_.has_consent()) {
            done(false, {});
            return;
        }
        const std::string id = client_.distinct_id();
        const std::string body = request_body(id, o, keys);
        client_.transport().request(client_.config().host + "/v1/flags", body, [done](int status, const std::string& text) {
            std::vector<FlagResult> flags;
            done(status >= 200 && status < 300 && parse(text, flags), flags);
        });
    }

    void evaluate_all(const FlagOptions& o, AllCallback done) { evaluate(o, {}, std::move(done)); }

    /// One flag. `found` is false when the gateway does not know it or the call failed. A found flag records its
    /// exposure (unless `o.send_exposure` is false), once per (distinct id, flag, value) for the life of this object.
    void get(const std::string& key, const FlagOptions& o, OneCallback done) {
        const std::string id = client_.distinct_id();
        evaluate(o, {key}, [this, key, id, o, done](bool ok, const std::vector<FlagResult>& flags) {
            for (const auto& f : flags) {
                if (!ok || f.key != key) continue;
                if (o.send_exposure) expose(id, f);
                done(true, f);
                return;
            }
            done(false, FlagResult());
        });
    }

    void is_enabled(const std::string& key, const FlagOptions& o, std::function<void(bool known, bool enabled)> done) {
        get(key, o, [done](bool found, const FlagResult& f) { done(found, found && f.enabled()); });
    }

    /// Parses a `/v1/flags` response body: `{"flags": {key: {enabled, value, variant, payload, reason, condition_index}},
    /// "request_id": ...}`. The value is `value`, else `variant`, else `enabled`.
    static bool parse(const std::string& text, std::vector<FlagResult>& out) {
        Value root;
        if (!detail::parse_json(text, root) || !std::holds_alternative<Object>(root.v)) return false;
        const Value* flags = detail::member(root, "flags");
        const Value* req = detail::member(root, "request_id");
        const std::string* request_id = req ? std::get_if<std::string>(&req->v) : nullptr;
        if (!flags) return true; // a valid response that decided nothing
        const Object* all = std::get_if<Object>(&flags->v);
        if (!all) return false;
        for (const auto& kv : *all) {
            FlagResult r;
            r.key = kv.first;
            const Value* value = detail::member(kv.second, "value");
            const Value* variant = detail::member(kv.second, "variant");
            const Value* enabled = detail::member(kv.second, "enabled");
            if (value && !std::holds_alternative<std::nullptr_t>(value->v)) r.value = *value;
            else if (variant && std::holds_alternative<std::string>(variant->v)) r.value = *variant;
            else r.value = Value(enabled && std::get_if<bool>(&enabled->v) ? *std::get_if<bool>(&enabled->v) : false);
            if (const Value* p = detail::member(kv.second, "payload")) r.payload = *p;
            if (const Value* why = detail::member(kv.second, "reason"))
                if (const std::string* s = std::get_if<std::string>(&why->v)) r.reason = *s;
            if (const Value* ci = detail::member(kv.second, "condition_index"))
                if (const int64_t* n = std::get_if<int64_t>(&ci->v)) r.condition_index = static_cast<int>(*n);
            if (request_id) r.request_id = *request_id;
            out.push_back(std::move(r));
        }
        return true;
    }

private:
    static constexpr size_t kSeenMax = 10000;

    std::string request_body(const std::string& id, const FlagOptions& o, const std::vector<std::string>& keys) {
        Props groups;
        for (const auto& kv : o.groups) groups[kv.first] = kv.second;
        std::string b = "{\"api_key\":";
        detail::json_str(b, client_.config().api_key);
        b += ",\"distinct_id\":";
        detail::json_str(b, id);
        const std::string anon = client_.anon_distinct_id();
        if (!anon.empty()) {
            b += ",\"anon_distinct_id\":";
            detail::json_str(b, anon);
        }
        b += ",\"groups\":";
        detail::json_props(b, groups);
        b += ",\"person_properties\":";
        detail::json_props(b, o.person_properties);
        b += ",\"group_properties\":";
        detail::json_props(b, o.group_properties);
        if (!o.app_version.empty()) {
            b += ",\"app_version\":";
            detail::json_str(b, o.app_version);
        }
        if (!o.bucketing_key.empty()) {
            b += ",\"bucketing_key\":";
            detail::json_str(b, o.bucketing_key);
        }
        append_list(b, "flag_keys", keys);
        append_list(b, "app_contexts", o.app_contexts);
        b += '}';
        return b;
    }

    static void append_list(std::string& b, const char* name, const std::vector<std::string>& items) {
        if (items.empty()) return;
        b += ",\"";
        b += name;
        b += "\":[";
        for (size_t i = 0; i < items.size(); i++) {
            if (i) b += ',';
            detail::json_str(b, items[i]);
        }
        b += ']';
    }

    void expose(const std::string& id, const FlagResult& f) {
        Props p;
        p["$feature_flag"] = f.key;
        p["$feature_flag_response"] = f.value;
        p["locally_evaluated"] = false;
        if (!f.request_id.empty()) p["$feature_flag_request_id"] = f.request_id;
        std::string fingerprint = id + '\0' + f.key + '\0';
        detail::json_value(fingerprint, f.value);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (seen_.size() >= kSeenMax) seen_.clear();
            if (!seen_.insert(fingerprint).second) return;
        }
        client_.capture("$feature_flag_called", p);
        client_.capture("$exposure", p);
    }

    Client& client_;
    std::mutex mu_;
    std::set<std::string> seen_;
};

} // namespace lucida
