# Lucida for C and C++

## Install

From GitHub, [lucidaanalytics/lucida-cpp](https://github.com/lucidaanalytics/lucida-cpp), release `v0.1.1`: download `lucida-cpp-v0.1.1.zip` from the [release page](https://github.com/lucidaanalytics/lucida-cpp/releases/tag/v0.1.1), unzip it, and add its `include/` directory to your include path (compile `src/lucida_c.cpp` too for the C API).


Header-only C++17 client (`include/lucida/lucida.hpp`, no exceptions or RTTI needed) plus a C API
(`include/lucida/lucida.h`, compile `src/lucida_c.cpp`). The Unreal plugin (`sdks/unreal`) is built on it.

```cpp
#include "lucida/lucida.hpp"

struct MyHttp : lucida::Transport {          // your HTTP stack; call done(status) once, any thread
    void post(const std::string& url, const std::string& body, std::function<void(int)> done) override;
};
MyHttp http;
lucida::MemoryStorage store;                  // or your own lucida::Storage (file, settings)
lucida::Config cfg;
cfg.api_key = "lc_pk_...";
cfg.consent = lucida::ConsentMode::OptIn;     // nothing is captured or stored before opt_in()
lucida::Client client(cfg, http, store);
client.opt_in();
client.capture("level_complete", {{"level", int64_t(3)}});
client.identify("user-42");
// every frame or on a timer:
client.tick();
```

- Events go to `POST /v1/batch` (20 events or 5 s, at most 100 per request, one batch in flight), each with a
  uuid v7 for dedupe. 408, 429, 5xx and network failures retry with exponential backoff and jitter (5 tries);
  other 4xx drop the batch. The queue holds 1000 events (oldest dropped) and is mirrored to `Storage` once consent is held (`persist_queue`), so a restart resends it.
- Identity (anon id, distinct id) and the consent decision go through `Storage` only once analytics consent is
  held; `opt_out()` sends `$opt_out`, forgets the identity and keeps only the refusal. `$lc_*` properties are dropped.
- Property values are JSON: null, bool, int64, double, string, and nested `lucida::List` / `lucida::Object` (16 levels).

- `client.alias("old-name")` sends `$create_alias`; `client.group_identify("org", "acme", {{"plan", "pro"}})` sends
  `$groupidentify` (its distinct id is `$org_acme` unless you pass an owner); `client.capture_in_groups(event, {{"org", "acme"}})`
  adds `$groups`. C API: `lucida_alias`, `lucida_group_identify`.
- Remote flag evaluation (`#include "lucida/flags.hpp"`): `lucida::Flags flags(client); flags.get("new-nav", opts, cb)` posts
  `/v1/flags` and records `$feature_flag_called` + `$exposure` once per (id, flag, value). It needs a Transport that
  overrides `request()` (returns the response body); a post-only transport reports failure. Nothing is sent without consent.
  C API: set `lucida_options.request`, call `lucida_response_done`, then `lucida_evaluate_flags`. Local evaluation is not
  in this SDK: the gateway decides every flag.

Tests: `make -C sdks/cpp test` (clang++/clang; builds with `-fno-exceptions -fno-rtti -Werror`, the way Unreal
compiles plugin code). `make -C sdks/cpp clean` removes `build/`.

---

This repository is a read-only mirror of the Lucida monorepo (synced by a script, never edited by hand); releases and provenance attestations are built here by GitHub Actions. Issues are welcome, changes land upstream.
