// alias, group identify, JSON reader and remote flag evaluation: `make -C sdks/cpp test`.
#include <cstdio>
#include <string>
#include <vector>

#include "lucida/flags.hpp"
#include "lucida/json.hpp"
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

// post() records batches; request() records flag calls and answers with the scripted status/body.
struct Http : lucida::Transport {
    std::vector<std::string> batches, flag_bodies, flag_urls;
    int flag_status = 200;
    std::string flag_reply;

    void post(const std::string&, const std::string& body, std::function<void(int)> done) override {
        batches.push_back(body);
        done(200);
    }
    void request(const std::string& url, const std::string& body, std::function<void(int, const std::string&)> done) override {
        flag_urls.push_back(url);
        flag_bodies.push_back(body);
        done(flag_status, flag_reply);
    }
};

// A transport that only implements post(): the default request() reports a network failure.
struct PostOnly : lucida::Transport {
    void post(const std::string&, const std::string&, std::function<void(int)> done) override { done(200); }
};

static lucida::Config cfg() {
    lucida::Config c;
    c.api_key = "lc_pk_test";
    c.host = "https://example.test";
    c.flush_at = 1000;
    return c;
}

static const char* kReply =
    "{\"flags\":{\"new-nav\":{\"enabled\":true,\"value\":true,\"reason\":\"condition_match\",\"condition_index\":0},"
    "\"checkout\":{\"enabled\":true,\"variant\":\"b\",\"payload\":{\"cta\":\"Buy \\u00e9\",\"n\":[1,2.5]},\"reason\":\"rollout\"},"
    "\"off\":{\"enabled\":false}},\"request_id\":\"req-1\"}";

static void test_json_reader() {
    lucida::Value v;
    CHECK(lucida::detail::parse_json("{\"a\":[1,-2,3.5,1e2,true,null,\"x\\ny\\ud83d\\ude00\"],\"b\":{}}", v));
    const lucida::Value* a = lucida::detail::member(v, "a");
    CHECK(a != nullptr && std::get<lucida::List>(a->v).size() == 7);
    const lucida::List& l = std::get<lucida::List>(a->v);
    CHECK(std::get<int64_t>(l[1].v) == -2 && std::get<double>(l[2].v) == 3.5 && std::get<double>(l[3].v) == 100.0);
    CHECK(std::get<std::string>(l[6].v) == "x\ny\xF0\x9F\x98\x80"); // surrogate pair -> U+1F600
    lucida::Value bad;
    CHECK(!lucida::detail::parse_json("{\"a\":", bad));
    CHECK(!lucida::detail::parse_json("[1,2,]", bad));
    CHECK(!lucida::detail::parse_json("{} trailing", bad));
    CHECK(!lucida::detail::parse_json(std::string(200, '['), bad)); // nesting is capped
}

static void test_alias_and_group_identify() {
    Http http;
    lucida::MemoryStorage store;
    lucida::Client c(cfg(), http, store);
    const std::string anon = c.distinct_id();
    CHECK(c.alias("old-name"));
    CHECK(c.alias("other", "user-9"));
    CHECK(!c.alias(""));
    CHECK(c.group_identify("org", "acme", {{"plan", std::string("pro")}, {"seats", int64_t(5)}, {"$lc_x", std::string("spoof")}}));
    CHECK(c.group_identify("org", "acme", {}, "user-9"));
    CHECK(!c.group_identify("", "acme"));
    CHECK(c.capture_in_groups("seat_added", {{"org", "acme"}}));
    c.flush();
    CHECK(http.batches.size() == 1);
    const std::string& b = http.batches[0];
    CHECK(has(b, "\"event\":\"$create_alias\""));
    CHECK(has(b, "\"alias\":\"old-name\",\"distinct_id\":\"" + anon + "\""));
    CHECK(has(b, "\"distinct_id\":\"user-9\",\"timestamp\"")); // alias / owner events carry the explicit id
    CHECK(has(b, "\"event\":\"$groupidentify\",\"distinct_id\":\"$org_acme\""));
    CHECK(has(b, "\"$group_type\":\"org\"") && has(b, "\"$group_key\":\"acme\""));
    CHECK(has(b, "\"$group_set\":{\"plan\":\"pro\",\"seats\":5}"));
    CHECK(!has(b, "$lc_x"));
    CHECK(has(b, "\"$groups\":{\"org\":\"acme\"}"));
}

static void test_flag_evaluation() {
    Http http;
    http.flag_reply = kReply;
    lucida::MemoryStorage store;
    lucida::Client c(cfg(), http, store);
    lucida::Flags flags(c);
    lucida::FlagOptions o;
    o.person_properties = {{"plan", std::string("pro")}};
    o.groups = {{"org", "acme"}};
    o.app_contexts = {"web"};

    std::vector<lucida::FlagResult> got;
    bool ok = false;
    flags.evaluate_all(o, [&](bool k, const std::vector<lucida::FlagResult>& f) { ok = k; got = f; });
    CHECK(ok && got.size() == 3);
    CHECK(http.flag_urls[0] == "https://example.test/v1/flags");
    const std::string& body = http.flag_bodies[0];
    CHECK(has(body, "\"api_key\":\"lc_pk_test\"") && has(body, "\"distinct_id\":\"" + c.distinct_id() + "\""));
    CHECK(has(body, "\"groups\":{\"org\":\"acme\"}") && has(body, "\"person_properties\":{\"plan\":\"pro\"}"));
    CHECK(has(body, "\"app_contexts\":[\"web\"]") && !has(body, "flag_keys") && !has(body, "anon_distinct_id"));
    for (const auto& f : got) {
        if (f.key == "new-nav") CHECK(f.enabled() && f.variant().empty() && f.reason == "condition_match" && f.condition_index == 0 && f.request_id == "req-1");
        if (f.key == "checkout") {
            CHECK(f.enabled() && f.variant() == "b");
            const lucida::Value* cta = lucida::detail::member(f.payload, "cta");
            CHECK(cta && std::get<std::string>(cta->v) == "Buy \xC3\xA9");
        }
        if (f.key == "off") CHECK(!f.enabled());
    }

    // one flag: sends flag_keys, records the exposure once per (id, flag, value)
    bool found = false;
    for (int i = 0; i < 2; i++) flags.get("checkout", o, [&](bool fnd, const lucida::FlagResult& f) { found = fnd && f.variant() == "b"; });
    CHECK(found && has(http.flag_bodies[1], "\"flag_keys\":[\"checkout\"]"));
    c.flush();
    std::string all;
    for (const auto& b : http.batches) all += b;
    size_t called = 0;
    for (size_t at = all.find("\"$feature_flag_called\""); at != std::string::npos; at = all.find("\"$feature_flag_called\"", at + 1)) called++;
    CHECK(called == 1);
    CHECK(has(all, "\"event\":\"$exposure\"") && has(all, "\"$feature_flag\":\"checkout\"") && has(all, "\"$feature_flag_response\":\"b\""));
    CHECK(has(all, "\"$feature_flag_request_id\":\"req-1\""));

    // unknown flag, no exposure option, is_enabled
    bool unknown_found = true;
    flags.get("missing", o, [&](bool fnd, const lucida::FlagResult&) { unknown_found = fnd; });
    CHECK(!unknown_found);
    lucida::FlagOptions quiet = o;
    quiet.send_exposure = false;
    bool known = false, enabled = true;
    flags.is_enabled("off", quiet, [&](bool k, bool e) { known = k; enabled = e; });
    CHECK(known && !enabled);
}

static void test_flag_failures() {
    lucida::MemoryStorage store;
    {
        Http http;
        http.flag_status = 500;
        http.flag_reply = kReply;
        lucida::Client c(cfg(), http, store);
        lucida::Flags flags(c);
        bool ok = true;
        flags.evaluate_all({}, [&](bool k, const std::vector<lucida::FlagResult>&) { ok = k; });
        CHECK(!ok); // a 5xx answer is a failure whatever the body says
        http.flag_status = 200;
        http.flag_reply = "not json";
        flags.evaluate_all({}, [&](bool k, const std::vector<lucida::FlagResult>&) { ok = k; });
        CHECK(!ok);
    }
    {
        PostOnly http; // no request(): reports failure, never crashes
        lucida::Client c(cfg(), http, store);
        lucida::Flags flags(c);
        bool ok = true;
        flags.evaluate_all({}, [&](bool k, const std::vector<lucida::FlagResult>&) { ok = k; });
        CHECK(!ok);
    }
    {
        Http http; // consent not granted: nothing is sent
        http.flag_reply = kReply;
        lucida::MemoryStorage fresh;
        lucida::Config c2 = cfg();
        c2.consent = lucida::ConsentMode::OptIn;
        lucida::Client c(c2, http, fresh);
        lucida::Flags flags(c);
        bool ok = true;
        flags.evaluate_all({}, [&](bool k, const std::vector<lucida::FlagResult>&) { ok = k; });
        CHECK(!ok && http.flag_bodies.empty());
    }
    {
        Http http; // after identify the anon id travels as anon_distinct_id
        http.flag_reply = "{\"flags\":{}}";
        lucida::MemoryStorage fresh;
        lucida::Client c(cfg(), http, fresh);
        const std::string anon = c.distinct_id();
        c.identify("user-1");
        lucida::Flags flags(c);
        bool ok = false;
        flags.evaluate_all({}, [&](bool k, const std::vector<lucida::FlagResult>& f) { ok = k && f.empty(); });
        CHECK(ok && has(http.flag_bodies[0], "\"distinct_id\":\"user-1\"") && has(http.flag_bodies[0], "\"anon_distinct_id\":\"" + anon + "\""));
    }
}

int main() {
    test_json_reader();
    test_alias_and_group_identify();
    test_flag_evaluation();
    test_flag_failures();
    std::printf("lucida c++ flag tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
