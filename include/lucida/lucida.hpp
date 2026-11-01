// Lucida C++ SDK: header-only, C++17, no exceptions or RTTI needed (so it builds inside Unreal).
// Engine-free core: identity, consent gate, event building, batching with retry/backoff, uuid v7.
// Bytes leave only through the Transport port; persistence only through the Storage port, and only
// once analytics consent is held. Wire format matches every Lucida SDK: POST <host>/v1/batch with
// {api_key, batch, sent_at}; each event carries a uuid v7 so a retried batch is deduplicated.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lucida {

using Value = std::variant<std::nullptr_t, bool, int64_t, double, std::string>;
// shortcut: flat property values only (no nested objects/arrays), add a JSON value type when a caller needs nesting.
using Props = std::map<std::string, Value>;

/// Port: sends a POST. `done(status)` may run inline or later, exactly once; status 0 = network failure.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void post(const std::string& url, const std::string& body, std::function<void(int)> done) = 0;
};

/// Port: string key/value persistence (a file, PlayerPrefs-like settings, a save game).
class Storage {
public:
    virtual ~Storage() = default;
    virtual std::string get(const std::string& key) = 0; // "" when missing
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& key) = 0;
};

class MemoryStorage : public Storage {
public:
    std::map<std::string, std::string> data;
    std::string get(const std::string& key) override {
        auto it = data.find(key);
        return it == data.end() ? std::string() : it->second;
    }
    void set(const std::string& key, const std::string& value) override { data[key] = value; }
    void remove(const std::string& key) override { data.erase(key); }
};

enum class ConsentMode { OptOut, OptIn };

struct Config {
    std::string api_key;
    std::string host = "https://app.lucida.dev";
    /// OptOut (default): capture until opt_out(). OptIn: nothing is captured or stored before opt_in().
    ConsentMode consent = ConsentMode::OptOut;
    size_t flush_at = 20;
    int64_t flush_interval_ms = 5000;
    size_t max_queue = 1000;
    size_t max_batch = 100;
    int max_retries = 5;
    std::string lib = "lucida-cpp";
    std::string lib_version = "0.1.0";
    /// Extra properties on every event (e.g. $os, $app_version); `$lc_*` keys are dropped.
    Props super_props;
    std::function<int64_t()> now_ms;  // default: system clock
    std::function<uint64_t()> random; // default: mt19937_64 seeded from random_device
};

namespace detail {

inline void json_str(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

inline void json_value(std::string& out, const Value& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) {
        out += "null";
    } else if (const bool* b = std::get_if<bool>(&v)) {
        out += *b ? "true" : "false";
    } else if (const int64_t* i = std::get_if<int64_t>(&v)) {
        out += std::to_string(*i);
    } else if (const double* d = std::get_if<double>(&v)) {
        if (*d != *d || *d > 1.7976931348623157e308 || *d < -1.7976931348623157e308) {
            out += "null"; // NaN and infinities are not JSON
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", *d);
            out += buf;
        }
    } else {
        json_str(out, std::get<std::string>(v));
    }
}

inline void json_props(std::string& out, const Props& p) {
    out += '{';
    bool first = true;
    for (const auto& kv : p) {
        if (!first) out += ',';
        first = false;
        json_str(out, kv.first);
        out += ':';
        json_value(out, kv.second);
    }
    out += '}';
}

/// `$lc_*` keys are reserved for the gateway (it strips them too).
inline Props strip_reserved(const Props& p) {
    Props out;
    for (const auto& kv : p)
        if (kv.first.compare(0, 4, "$lc_") != 0) out.insert(kv);
    return out;
}

/// RFC 3339 UTC with milliseconds, from unix ms (days-from-civil inverse; no gmtime, so no locale or TZ).
inline std::string iso8601(int64_t ms) {
    int64_t secs = ms / 1000, milli = ms % 1000;
    if (milli < 0) { milli += 1000; secs -= 1; }
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(rem / 3600),
                  static_cast<long long>(rem / 60 % 60), static_cast<long long>(rem % 60), static_cast<long long>(milli));
    return buf;
}

/// UUID v7: 48-bit unix ms, version 7, RFC 4122 variant, 74 random bits.
inline std::string uuid_v7(int64_t ms, const std::function<uint64_t()>& rnd) {
    unsigned char b[16];
    const uint64_t t = static_cast<uint64_t>(ms);
    for (int i = 0; i < 6; i++) b[i] = static_cast<unsigned char>(t >> (8 * (5 - i)));
    const uint64_t r1 = rnd(), r2 = rnd();
    for (int i = 6; i < 16; i++) b[i] = static_cast<unsigned char>((i < 11 ? r1 : r2) >> (8 * (i % 8)));
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x70);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 0x0f];
    }
    return s;
}

/// 408, 429, 5xx and network failures (0) are retried; any other non-2xx drops the batch.
inline bool retryable(int status) { return status == 0 || status == 408 || status == 429 || status >= 500; }

/// Exponential backoff with jitter: base * 2^attempt (capped), scaled by [0.5, 1).
inline int64_t backoff_ms(int attempt, double r01, int64_t base = 1000, int64_t cap = 30000) {
    int64_t d = base;
    for (int i = 0; i < attempt && d < cap; i++) d *= 2;
    if (d > cap) d = cap;
    return static_cast<int64_t>(static_cast<double>(d) * (0.5 + 0.5 * r01));
}

} // namespace detail

/// One Lucida client. Thread-safe; the Transport callback may arrive on any thread.
/// Keep the Client (and its Transport/Storage) alive until in-flight requests completed (see shutdown()).
class Client {
public:
    static constexpr int64_t kSessionIdleMs = 30LL * 60 * 1000;

    Client(Config cfg, Transport& transport, Storage& storage)
        : cfg_(std::move(cfg)), transport_(transport), storage_(storage) {
        while (!cfg_.host.empty() && cfg_.host.back() == '/') cfg_.host.pop_back();
        if (!cfg_.now_ms) {
            cfg_.now_ms = [] {
                return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
            };
        }
        if (!cfg_.random) {
            auto eng = std::make_shared<std::mt19937_64>(std::random_device{}());
            cfg_.random = [eng] { return (*eng)(); };
        }
        const std::string saved = storage_.get("lucida.consent");
        consent_ = saved.empty() ? cfg_.consent == ConsentMode::OptOut : saved == "granted";
        anon_id_ = storage_.get("lucida.anon_id");
        if (anon_id_.empty()) anon_id_ = detail::uuid_v7(cfg_.now_ms(), cfg_.random);
        distinct_id_ = storage_.get("lucida.distinct_id");
        if (distinct_id_.empty()) distinct_id_ = anon_id_;
        identified_ = storage_.get("lucida.identified") == "1";
        last_flush_ = cfg_.now_ms();
        persist();
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Queues an event; returns false when consent blocks it.
    bool capture(const std::string& event, const Props& props = {}) { return enqueue(event, props, {}, {}); }

    bool screen(const std::string& name, Props props = {}) {
        props["$screen_name"] = name;
        return enqueue("$screen", props, {}, {});
    }

    /// Links the anonymous id to `user_id` once; later calls only switch ids (no merge of two known users).
    bool identify(const std::string& user_id, const Props& set = {}, const Props& set_once = {}) {
        Props p;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (user_id.empty() || !consent_) return false;
            if (!identified_ && distinct_id_ != user_id) p["$anon_distinct_id"] = distinct_id_;
            distinct_id_ = user_id;
            identified_ = true;
            persist_locked();
        }
        return enqueue("$identify", p, set, set_once);
    }

    /// New anonymous identity and session (call on logout).
    void reset() {
        std::lock_guard<std::mutex> lock(mu_);
        anon_id_ = detail::uuid_v7(cfg_.now_ms(), cfg_.random);
        distinct_id_ = anon_id_;
        identified_ = false;
        session_id_.clear();
        persist_locked();
    }

    void opt_in() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (consent_) return;
            consent_ = true;
            persist_locked();
        }
        capture("$opt_in");
    }

    /// Sends `$opt_out` with what is already queued, then forgets the stored identity.
    void opt_out() {
        if (has_consent()) capture("$opt_out");
        {
            std::lock_guard<std::mutex> lock(mu_);
            consent_ = false;
            storage_.remove("lucida.anon_id");
            storage_.remove("lucida.distinct_id");
            storage_.remove("lucida.identified");
            storage_.set("lucida.consent", "denied"); // the refusal itself is the one thing kept
        }
        flush();
    }

    bool has_consent() const {
        std::lock_guard<std::mutex> lock(mu_);
        return consent_;
    }

    std::string distinct_id() const {
        std::lock_guard<std::mutex> lock(mu_);
        return distinct_id_;
    }

    size_t queued() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.size();
    }

    size_t dropped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }

    bool in_flight() const {
        std::lock_guard<std::mutex> lock(mu_);
        return in_flight_ > 0;
    }

    /// Call every frame (or on a timer): flushes once the interval passed or a retry backoff ended.
    void tick() {
        bool due;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const int64_t now = cfg_.now_ms();
            due = !queue_.empty() && in_flight_ == 0 &&
                  (retry_at_ != 0 ? now >= retry_at_ : now - last_flush_ >= cfg_.flush_interval_ms);
        }
        if (due) flush();
    }

    /// Sends the next batch unless one is in flight or a backoff is pending.
    void flush() {
        std::string body;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const int64_t now = cfg_.now_ms();
            last_flush_ = now;
            if (in_flight_ > 0 || queue_.empty() || now < retry_at_) return;
            n = queue_.size() < cfg_.max_batch ? queue_.size() : cfg_.max_batch;
            body = "{\"api_key\":";
            detail::json_str(body, cfg_.api_key);
            body += ",\"sent_at\":";
            detail::json_str(body, detail::iso8601(now));
            body += ",\"batch\":[";
            for (size_t i = 0; i < n; i++) {
                if (i) body += ',';
                body += queue_[i];
            }
            body += "]}";
            in_flight_ = n;
        }
        transport_.post(cfg_.host + "/v1/batch", body, [this, n](int status) { settle(n, status); });
    }

    /// Best-effort final send (app quit): ignores the backoff once.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            retry_at_ = 0;
        }
        flush();
    }

private:
    bool enqueue(const std::string& event, const Props& props, const Props& set, const Props& set_once) {
        bool full;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!consent_) return false;
            const int64_t now = cfg_.now_ms();
            if (session_id_.empty() || now - session_last_ > kSessionIdleMs) session_id_ = detail::uuid_v7(now, cfg_.random);
            session_last_ = now;
            Props p = detail::strip_reserved(cfg_.super_props);
            for (const auto& kv : detail::strip_reserved(props)) p[kv.first] = kv.second;
            p["$lib"] = cfg_.lib;
            p["$lib_version"] = cfg_.lib_version;
            p["$session_id"] = session_id_;
            std::string e = "{\"uuid\":";
            detail::json_str(e, detail::uuid_v7(now, cfg_.random));
            e += ",\"event\":";
            detail::json_str(e, event);
            e += ",\"distinct_id\":";
            detail::json_str(e, distinct_id_);
            e += ",\"timestamp\":";
            detail::json_str(e, detail::iso8601(now));
            e += ",\"properties\":";
            detail::json_props(e, p);
            if (!set.empty()) {
                e += ",\"$set\":";
                detail::json_props(e, detail::strip_reserved(set));
            }
            if (!set_once.empty()) {
                e += ",\"$set_once\":";
                detail::json_props(e, detail::strip_reserved(set_once));
            }
            e += '}';
            queue_.push_back(std::move(e));
            // Over the cap: drop the oldest event that is not part of the batch in flight.
            if (queue_.size() > cfg_.max_queue) {
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(in_flight_ < queue_.size() - 1 ? in_flight_ : queue_.size() - 1));
                dropped_++;
            }
            full = queue_.size() >= cfg_.flush_at;
        }
        if (full) flush();
        return true;
    }

    void settle(size_t n, int status) {
        bool more;
        {
            std::lock_guard<std::mutex> lock(mu_);
            in_flight_ = 0;
            if (n > queue_.size()) n = queue_.size();
            if (status >= 200 && status < 300) {
                queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
                attempt_ = 0;
                retry_at_ = 0;
            } else if (detail::retryable(status) && attempt_ < cfg_.max_retries) {
                const double r01 = static_cast<double>(cfg_.random() % 1000000) / 1000000.0;
                retry_at_ = cfg_.now_ms() + detail::backoff_ms(attempt_, r01);
                attempt_++;
            } else {
                queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
                dropped_ += n;
                attempt_ = 0;
                retry_at_ = 0;
            }
            more = retry_at_ == 0 && queue_.size() >= cfg_.flush_at;
        }
        if (more) flush();
    }

    void persist() {
        std::lock_guard<std::mutex> lock(mu_);
        persist_locked();
    }

    // Nothing reaches Storage before analytics consent (opt_in mode keeps the anon id in memory only).
    void persist_locked() {
        if (!consent_) return;
        storage_.set("lucida.consent", "granted");
        storage_.set("lucida.anon_id", anon_id_);
        storage_.set("lucida.distinct_id", distinct_id_);
        storage_.set("lucida.identified", identified_ ? "1" : "0");
    }

    Config cfg_;
    Transport& transport_;
    Storage& storage_;
    mutable std::mutex mu_;
    bool consent_ = false;
    bool identified_ = false;
    std::string anon_id_, distinct_id_, session_id_;
    int64_t session_last_ = 0, last_flush_ = 0, retry_at_ = 0;
    int attempt_ = 0;
    size_t in_flight_ = 0, dropped_ = 0;
    // shortcut: unsent events live in memory only; persist the queue through Storage if games need offline replay of events.
    std::deque<std::string> queue_;
};

} // namespace lucida
