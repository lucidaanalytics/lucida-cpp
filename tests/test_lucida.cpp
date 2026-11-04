// Unit tests for the header-only core: `make -C sdks/cpp test`.
#include <cstdio>
#include <string>
#include <vector>

#include "lucida/lucida.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// Records each POST; answers inline with the next scripted status (default 200), or defers when `hold` is set.
struct FakeTransport : lucida::Transport {
    struct Call {
        std::string url, body;
    };
    std::vector<Call> calls;
    std::vector<int> script;
    bool hold = false;
    std::vector<std::function<void(int)>> pending;

    void post(const std::string& url, const std::string& body, std::function<void(int)> done) override {
        calls.push_back({url, body});
        if (hold) {
            pending.push_back(std::move(done));
            return;
        }
        int status = 200;
        if (!script.empty()) {
            status = script.front();
            script.erase(script.begin());
        }
        done(status);
    }
};

struct Env {
    int64_t now = 1767225600000; // 2026-01-01T00:00:00Z
    uint64_t seed = 0x0123456789abcdefULL;
    FakeTransport http;
    lucida::MemoryStorage store;

    lucida::Config cfg(lucida::ConsentMode mode = lucida::ConsentMode::OptOut, size_t flush_at = 100) {
        lucida::Config c;
        c.api_key = "lc_pk_test";
        c.host = "https://example.test/";
        c.consent = mode;
        c.flush_at = flush_at;
        c.now_ms = [this] { return now; };
        c.random = [this] { return seed += 0x9e3779b97f4a7c15ULL; };
        return c;
    }
};

static void test_helpers() {
    CHECK(lucida::detail::iso8601(1767225600000) == "2026-01-01T00:00:00.000Z");
    CHECK(lucida::detail::iso8601(951782400123) == "2000-02-29T00:00:00.123Z");
    CHECK(lucida::detail::iso8601(0) == "1970-01-01T00:00:00.000Z");
    uint64_t n = 0;
    std::string u = lucida::detail::uuid_v7(1767225600000, [&n] { return n += 0xffffffffffffULL; });
    CHECK(u.size() == 36 && u[8] == '-' && u[14] == '7');
    CHECK(u.substr(0, 13) == "019b76da-a800"); // 48-bit ms prefix
    CHECK(u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
    CHECK(lucida::detail::retryable(0) && lucida::detail::retryable(429) && lucida::detail::retryable(503));
    CHECK(!lucida::detail::retryable(400) && !lucida::detail::retryable(404));
    CHECK(lucida::detail::backoff_ms(0, 0.0) == 500 && lucida::detail::backoff_ms(10, 0.999) <= 30000);
    std::string j;
    lucida::detail::json_props(j, {{"a\"b", std::string("x\ny")}, {"n", int64_t(3)}, {"f", 1.5}, {"t", true}, {"z", nullptr}});
    CHECK(j == "{\"a\\\"b\":\"x\\ny\",\"f\":1.5,\"n\":3,\"t\":true,\"z\":null}");
}

static void test_event_shape_and_batch() {
    Env env;
    lucida::Client c(env.cfg(lucida::ConsentMode::OptOut, 2), env.http, env.store);
    CHECK(c.capture("level_complete", {{"level", int64_t(3)}, {"$lc_project", std::string("spoof")}}));
    CHECK(env.http.calls.empty());
    CHECK(c.screen("Shop"));
    CHECK(env.http.calls.size() == 1);
    const auto& call = env.http.calls[0];
    CHECK(call.url == "https://example.test/v1/batch");
    CHECK(has(call.body, "\"api_key\":\"lc_pk_test\""));
    CHECK(has(call.body, "\"sent_at\":\"2026-01-01T00:00:00.000Z\""));
    CHECK(has(call.body, "\"event\":\"level_complete\""));
    CHECK(has(call.body, "\"level\":3"));
    CHECK(!has(call.body, "$lc_project"));
    CHECK(has(call.body, "\"$lib\":\"lucida-cpp\""));
    CHECK(has(call.body, "\"$session_id\":\""));
    CHECK(has(call.body, "\"$screen_name\":\"Shop\""));
    CHECK(has(call.body, "\"distinct_id\":\"" + c.distinct_id() + "\""));
    CHECK(c.queued() == 0);
}

static void test_opt_in_gates_capture_and_storage() {
    Env env;
    {
        lucida::Client c(env.cfg(lucida::ConsentMode::OptIn), env.http, env.store);
        CHECK(!c.capture("early"));
        CHECK(!c.identify("u1"));
        CHECK(c.queued() == 0);
        CHECK(env.store.data.empty()); // nothing stored before consent
        c.opt_in();
        CHECK(c.queued() == 1);
        CHECK(env.store.get("lucida.consent") == "granted");
        CHECK(env.store.get("lucida.anon_id") == c.distinct_id());
        c.identify("user-42");
        c.opt_out();
        CHECK(!c.capture("late"));
        CHECK(env.http.calls.size() == 1 && has(env.http.calls[0].body, "\"event\":\"$opt_out\""));
        CHECK(env.store.get("lucida.distinct_id").empty());
        CHECK(env.store.get("lucida.consent") == "denied");
    }
    lucida::Client again(env.cfg(lucida::ConsentMode::OptOut), env.http, env.store);
    CHECK(!again.has_consent()); // a stored refusal wins over the opt_out default
}

static void test_identity_persists_and_links_once() {
    Env env;
    std::string anon;
    {
        lucida::Client c(env.cfg(), env.http, env.store);
        anon = c.distinct_id();
        c.identify("user-42", {{"plan", std::string("pro")}});
        c.flush();
        CHECK(has(env.http.calls.back().body, "\"$anon_distinct_id\":\"" + anon + "\""));
        CHECK(has(env.http.calls.back().body, "\"$set\":{\"plan\":\"pro\"}"));
        c.identify("user-43");
        c.flush();
        CHECK(!has(env.http.calls.back().body, "$anon_distinct_id"));
    }
    lucida::Client restored(env.cfg(), env.http, env.store);
    CHECK(restored.distinct_id() == "user-43");
    restored.reset();
    CHECK(restored.distinct_id() != "user-43" && restored.distinct_id() != anon);
}

static void test_retry_backoff_and_drop() {
    Env env;
    lucida::Client c(env.cfg(), env.http, env.store);
    c.capture("a");
    env.http.script = {503};
    c.flush();
    CHECK(c.queued() == 1); // kept for retry
    c.flush();
    CHECK(env.http.calls.size() == 1); // still backing off
    env.now += 1000;
    c.tick();
    CHECK(env.http.calls.size() == 2 && c.queued() == 0);
    c.capture("bad");
    env.http.script = {400};
    c.flush();
    CHECK(c.queued() == 0 && c.dropped() == 1); // 4xx drops the batch
}

static void test_retries_give_up() {
    Env env;
    auto cfg = env.cfg();
    cfg.max_retries = 2;
    lucida::Client c(cfg, env.http, env.store);
    c.capture("a");
    env.http.script = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        c.shutdown(); // ignores the backoff
    }
    CHECK(env.http.calls.size() == 3 && c.queued() == 0 && c.dropped() == 1);
}

static void test_batches_and_queue_cap_with_deferred_transport() {
    Env env;
    auto cfg = env.cfg(lucida::ConsentMode::OptOut, 1000);
    cfg.max_queue = 150;
    lucida::Client c(cfg, env.http, env.store);
    env.http.hold = true;
    for (int i = 0; i < 120; i++) c.capture("e" + std::to_string(i));
    c.flush();
    CHECK(env.http.calls.size() == 1 && c.in_flight());
    c.flush();
    CHECK(env.http.calls.size() == 1); // one batch in flight at a time
    for (int i = 120; i < 160; i++) c.capture("e" + std::to_string(i));
    CHECK(c.queued() == 150 && c.dropped() == 10);
    auto done = env.http.pending.back();
    env.http.pending.clear();
    done(200);
    CHECK(c.queued() == 50); // the 100 in flight were never dropped by the cap
    const std::string& first = env.http.calls[0].body;
    CHECK(has(first, "\"event\":\"e0\"") && has(first, "\"event\":\"e99\"") && !has(first, "\"event\":\"e100\""));
    env.http.hold = false;
    c.flush();
    CHECK(c.queued() == 0);
    CHECK(!has(env.http.calls[1].body, "\"event\":\"e100\"") && has(env.http.calls[1].body, "\"event\":\"e159\""));
}

static void test_interval_flush_and_session_rotation() {
    Env env;
    lucida::Client c(env.cfg(), env.http, env.store);
    c.capture("a");
    c.tick();
    CHECK(env.http.calls.empty());
    env.now += 5000;
    c.tick();
    CHECK(env.http.calls.size() == 1);
    auto sid = [](const std::string& body) {
        auto at = body.find("\"$session_id\":\"") + 15;
        return body.substr(at, 36);
    };
    std::string s1 = sid(env.http.calls[0].body);
    env.now += lucida::Client::kSessionIdleMs + 1;
    c.capture("b");
    c.flush();
    CHECK(sid(env.http.calls[1].body) != s1);
}

static void test_nested_values() {
    std::string j;
    lucida::detail::json_props(j, {{"cart", lucida::Object{{"items", lucida::List{int64_t(1), std::string("a\"b"), nullptr}}, {"ok", true}}},
                                   {"n", 7}, {"s", "lit"}});
    CHECK(j == "{\"cart\":{\"items\":[1,\"a\\\"b\",null],\"ok\":true},\"n\":7,\"s\":\"lit\"}");
    lucida::Value deep = int64_t(1);
    for (int i = 0; i < 40; i++) deep = lucida::List{deep};
    std::string d;
    lucida::detail::json_value(d, deep);
    CHECK(d.find("null") != std::string::npos && d.size() < 100); // cut at kMaxDepth
}

static void test_queue_survives_restart() {
    Env env;
    {
        lucida::Client c(env.cfg(), env.http, env.store);
        c.capture("offline_1");
        c.capture("offline_2");
        env.http.script = {0};
        c.flush(); // network down: the events stay queued, and stored
        CHECK(c.queued() == 2);
    } // the process "dies" here
    lucida::Client again(env.cfg(), env.http, env.store);
    CHECK(again.queued() == 2);
    again.flush();
    CHECK(has(env.http.calls.back().body, "\"event\":\"offline_1\"") && has(env.http.calls.back().body, "\"event\":\"offline_2\""));
    CHECK(again.queued() == 0 && env.store.get("lucida.queue").empty());
    // opt-in mode stores nothing before consent; opting out erases what was stored
    Env gated;
    lucida::Client g(gated.cfg(lucida::ConsentMode::OptIn), gated.http, gated.store);
    g.opt_in();
    g.capture("x");
    gated.http.script = {0};
    g.flush();
    CHECK(!gated.store.get("lucida.queue").empty());
    g.opt_out();
    CHECK(gated.store.get("lucida.queue").empty());
}

int main() {
    test_nested_values();
    test_queue_survives_restart();
    test_helpers();
    test_event_shape_and_batch();
    test_opt_in_gates_capture_and_storage();
    test_identity_persists_and_links_once();
    test_retry_backoff_and_drop();
    test_retries_give_up();
    test_batches_and_queue_cap_with_deferred_transport();
    test_interval_flush_and_session_rotation();
    std::printf("lucida c++ tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
