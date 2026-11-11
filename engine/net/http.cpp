#include "net/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace zl {
namespace {

void ensure_curl_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { (void)curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct EasyHandle {
    CURL* handle = nullptr;

    EasyHandle() : handle(curl_easy_init()) {}
    EasyHandle(const EasyHandle&) = delete;
    EasyHandle& operator=(const EasyHandle&) = delete;
    ~EasyHandle() {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

struct HeaderList {
    curl_slist* list = nullptr;

    HeaderList() = default;
    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;
    ~HeaderList() {
        if (list != nullptr) {
            curl_slist_free_all(list);
        }
    }

    bool append(const std::string& line) {
        curl_slist* next = curl_slist_append(list, line.c_str());
        if (next == nullptr) {
            return false;
        }
        list = next;
        return true;
    }
};

struct WriteContext {
    std::string body;
    std::size_t max_bytes = 0;
    bool overflow = false;
};

size_t write_body(char* data, size_t size, size_t count, void* userdata) {
    auto* ctx = static_cast<WriteContext*>(userdata);
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        ctx->overflow = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (ctx->body.size() > ctx->max_bytes || bytes > ctx->max_bytes - ctx->body.size()) {
        ctx->overflow = true;
        return 0;
    }
    ctx->body.append(data, bytes);
    return bytes;
}

bool equals_ignore_case(const std::string& lhs, const char* rhs) {
    std::size_t i = 0;
    for (; i < lhs.size() && rhs[i] != '\0'; ++i) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[i]);
        const unsigned char lower_a =
            (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a - 'A' + 'a') : a;
        const unsigned char lower_b =
            (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b - 'A' + 'a') : b;
        if (lower_a != lower_b) {
            return false;
        }
    }
    return i == lhs.size() && rhs[i] == '\0';
}

}  // namespace

HttpResponse http_request(const HttpRequest& request) {
    HttpResponse response;

    if (request.timeout_ms < 0) {
        response.error = "timeout_ms must be >= 0";
        return response;
    }

    ensure_curl_initialized();

    EasyHandle easy;
    if (easy.handle == nullptr) {
        response.error = "failed to create curl easy handle";
        return response;
    }

    HeaderList headers;
    bool has_expect = false;
    for (const HttpHeader& header : request.headers) {
        if (header.name.empty() || header.name.find_first_of("\r\n") != std::string::npos ||
            header.name.find(':') != std::string::npos || header.name.find('\0') != std::string::npos ||
            header.value.find_first_of("\r\n") != std::string::npos ||
            header.value.find('\0') != std::string::npos) {
            response.error = "invalid HTTP header";
            return response;
        }
        if (equals_ignore_case(header.name, "Expect")) {
            has_expect = true;
        }
        if (!headers.append(header.name + ": " + header.value)) {
            response.error = "failed to allocate curl header list";
            return response;
        }
    }
    if (!has_expect && !headers.append("Expect:")) {
        response.error = "failed to allocate curl header list";
        return response;
    }

    WriteContext ctx;
    ctx.max_bytes = request.max_response_bytes;
    ctx.body.reserve(
        std::min<std::size_t>(request.max_response_bytes, std::size_t{64} * 1024));

    char error_buffer[CURL_ERROR_SIZE] = {};

    CURL* handle = easy.handle;
    (void)curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    (void)curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    if (request.follow_redirects) {
        (void)curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        (void)curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    }
    (void)curl_easy_setopt(handle, CURLOPT_USERAGENT, "zillions/0.1");
    (void)curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    (void)curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    (void)curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    (void)curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.list);
    (void)curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &write_body);
    (void)curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
    (void)curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);

    const long timeout_ms = static_cast<long>(request.timeout_ms);
    (void)curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout_ms);
    const long connect_timeout_ms =
        timeout_ms > 0 ? std::min<long>(timeout_ms, 15000L) : 15000L;
    (void)curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);

    if (request.method == HttpMethod::Post) {
        (void)curl_easy_setopt(handle, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
        (void)curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(request.body.size()));
    } else {
        (void)curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    }

    const CURLcode result = curl_easy_perform(handle);

    response.body = std::move(ctx.body);

    if (ctx.overflow) {
        response.status = 0;
        response.error =
            "response body exceeds " + std::to_string(request.max_response_bytes) + " bytes";
        return response;
    }

    if (result != CURLE_OK) {
        response.status = 0;
        if (error_buffer[0] != '\0') {
            response.error = error_buffer;
        } else {
            response.error = curl_easy_strerror(result);
        }
        return response;
    }

    long status_code = 0;
    if (curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code) == CURLE_OK) {
        response.status = static_cast<i32>(status_code);
    }
    return response;
}

}  // namespace zl
