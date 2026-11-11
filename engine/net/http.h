// Minimal synchronous HTTP(S) transport for external services.
#pragma once

#include <string>
#include <vector>

#include "core/base.h"

namespace zl {

struct HttpHeader {
    std::string name;
    std::string value;
};

enum class HttpMethod { Get, Post };

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    i32 timeout_ms = 60000;  // 0 disables the total timeout
    std::size_t max_response_bytes = 16u * 1024u * 1024u;
    // Off by default: a redirected POST may be replayed as a bodyless GET.
    bool follow_redirects = false;
};

struct HttpResponse {
    i32 status = 0;     // HTTP status, or 0 when the exchange did not complete
    std::string body;
    std::string error;  // transport error; empty on a completed HTTP exchange

    bool ok() const { return status >= 200 && status < 300; }
};

// Performs one blocking HTTP(S) request. Each call owns its connection, so it
// is safe to call from several threads at once. TLS verification and proxy
// environment variables follow libcurl's defaults.
HttpResponse http_request(const HttpRequest& request);

}  // namespace zl
