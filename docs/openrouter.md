# OpenRouter client

`game/ai/openrouter.{h,cpp}` is a small, blocking client for OpenRouter's
chat-completions endpoint. It is not wired into gameplay yet; it exists so
future model-driven features have one place to send requests and parse replies.

## Configuration

The client reads `OPENROUTER_API_KEY` when constructed with an empty key:

```sh
export OPENROUTER_API_KEY=sk-or-...
```

`OpenRouterConfig` also takes an explicit key, a different base URL, a timeout
and the optional `HTTP-Referer` / `X-OpenRouter-Title` attribution headers. The
base URL must use HTTPS; plaintext HTTP is accepted only for loopback hosts so
local tests and proxies can run without a certificate.

```cpp
#include "ai/openrouter.h"

zl::OpenRouterConfig config;
config.app_name = "Zillions";
config.app_url = "https://example.com/zillions";

zl::OpenRouterClient client(config);

zl::ChatRequest request;
request.model = "openai/gpt-4o-mini";
request.messages = {
    {zl::ChatRole::System, "You write terse colony event reports."},
    {zl::ChatRole::User, "A horde reached the north wall at dusk."},
};
request.temperature = 0.7f;
request.max_tokens = 200;

const zl::ChatResponse response = client.chat(request);
if (!response.ok) {
    // response.error, response.http_status and response.raw are populated.
} else {
    // response.content, response.reasoning, response.model, response.finish_reason
    // and response.usage.
}
```

`chat()` blocks until the response arrives. Call it from a worker thread, not
from the render or simulation loop. Streaming is not implemented; this client
handles one complete response per request.

## Build and tests

libcurl and nlohmann/json are fetched at configure time, like GLFW and stb. On
Linux, libcurl's OpenSSL backend needs the OpenSSL development package
(`libssl-dev` on Debian/Ubuntu). The client is part of the headless build, so
`-DZL_BUILD_APP=OFF` still builds it.

```sh
ctest --test-dir build -C Release -R zl_openrouter --output-on-failure
```

The tests use a fake transport for request/response behavior and a loopback
HTTP server for the libcurl path; they do not contact OpenRouter.

For a real request, set `ZL_LIVE_OPENROUTER` and run the same binary. The model
defaults to `liquid/lfm-3-1.7b`; override it with `ZL_OPENROUTER_MODEL`:

```sh
OPENROUTER_API_KEY=... ZL_LIVE_OPENROUTER=1 \
  ./build/tests/Release/zl_openrouter_tests.exe
```
