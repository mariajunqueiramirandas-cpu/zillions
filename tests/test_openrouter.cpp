#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <csignal>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "ai/openrouter.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace zl;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {

#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
using SocketLength = socklen_t;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { ::close(socket); }
#endif

// ---------------------------------------------------------------------------
// Fake transport
// ---------------------------------------------------------------------------

struct FakeState {
    bool called = false;
    HttpRequest request;
    HttpResponse response;
};

FakeState g_fake;

HttpResponse fake_transport(const HttpRequest& request) {
    g_fake.called = true;
    g_fake.request = request;
    return g_fake.response;
}

void fake_response(i32 status, std::string body, std::string error = {}) {
    g_fake = FakeState{};
    g_fake.response.status = status;
    g_fake.response.body = std::move(body);
    g_fake.response.error = std::move(error);
}

std::string header(const HttpRequest& request, const std::string& name) {
    for (const HttpHeader& entry : request.headers) {
        if (entry.name == name) return entry.value;
    }
    return {};
}

bool has_header(const HttpRequest& request, const std::string& name) {
    for (const HttpHeader& entry : request.headers) {
        if (entry.name == name) return true;
    }
    return false;
}

OpenRouterConfig config(std::string api_key = "test-key") {
    OpenRouterConfig value;
    value.api_key = std::move(api_key);
    return value;
}

ChatRequest simple_request() {
    ChatRequest request;
    request.model = "openai/gpt-4o-mini";
    request.messages.push_back({ChatRole::User, "hi"});
    return request;
}

std::string get_env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::string();
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

// ---------------------------------------------------------------------------
// Loopback HTTP server
// ---------------------------------------------------------------------------

struct Listener {
    Socket handle = kInvalidSocket;
    int port = 0;
};

Listener open_listener() {
    Listener listener;
    listener.handle = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener.handle != kInvalidSocket);
    int reuse = 1;
    CHECK(setsockopt(listener.handle, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse),
                     static_cast<SocketLength>(sizeof(reuse))) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(listener.handle, reinterpret_cast<const sockaddr*>(&address),
               static_cast<SocketLength>(sizeof(address))) == 0);
    CHECK(listen(listener.handle, 1) == 0);
    SocketLength length = static_cast<SocketLength>(sizeof(address));
    CHECK(getsockname(listener.handle, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    listener.port = static_cast<int>(ntohs(address.sin_port));
    return listener;
}

std::size_t parse_content_length(const std::string& header_block) {
    std::string lower = header_block;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string key = "\r\ncontent-length:";
    std::size_t pos = lower.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) ++pos;
    std::size_t value = 0;
    while (pos < lower.size() && lower[pos] >= '0' && lower[pos] <= '9') {
        value = value * 10 + static_cast<std::size_t>(lower[pos] - '0');
        ++pos;
    }
    return value;
}

std::string read_request(Socket client) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const auto received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) return data;
        data.append(buffer, static_cast<std::size_t>(received));
    }
    const std::size_t total = header_end + 4 + parse_content_length(data.substr(0, header_end));
    while (data.size() < total) {
        const auto received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) break;
        data.append(buffer, static_cast<std::size_t>(received));
    }
    return data;
}

void send_all(Socket client, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written = send(client, data.data() + sent,
                                  static_cast<int>(data.size() - sent), 0);
        if (written <= 0) return;
        sent += static_cast<std::size_t>(written);
    }
}

void serve_once(const Listener& listener, const std::string& response_body, std::string* captured) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener.handle, &read_set);
    timeval timeout{10, 0};
    const int ready = select(static_cast<int>(listener.handle + 1), &read_set, nullptr, nullptr, &timeout);
    if (ready <= 0) return;
    Socket client = accept(listener.handle, nullptr, nullptr);
    if (client == kInvalidSocket) return;
    const std::string request = read_request(client);
    if (captured != nullptr) *captured = request;
    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + response_body;
    send_all(client, response);
    close_socket(client);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void success() {
    fake_response(200, R"({"id":"gen-1","model":"openai/gpt-4o-mini",)"
                       R"("choices":[{"message":{"role":"assistant","content":"hello","reasoning":"think"},"finish_reason":"stop"}],)"
                       R"("usage":{"prompt_tokens":5,"completion_tokens":2,"total_tokens":7}})");
    OpenRouterClient client(config(), fake_transport);
    ChatRequest request;
    request.model = "openai/gpt-4o-mini";
    request.messages.push_back({ChatRole::System, "be terse"});
    request.messages.push_back({ChatRole::User, "hi"});
    request.temperature = 0.25f;
    request.max_tokens = 128;
    request.json_mode = true;

    const ChatResponse response = client.chat(request);
    CHECK(g_fake.called);
    CHECK(g_fake.request.method == HttpMethod::Post);
    CHECK(g_fake.request.url == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(g_fake.request.timeout_ms == 60000);
    CHECK(header(g_fake.request, "Authorization") == "Bearer test-key");
    CHECK(header(g_fake.request, "Content-Type") == "application/json");
    CHECK(header(g_fake.request, "Accept") == "application/json");

    const nlohmann::json body = nlohmann::json::parse(g_fake.request.body);
    CHECK(body["model"] == "openai/gpt-4o-mini");
    CHECK(body["messages"].size() == 2u);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][0]["content"] == "be terse");
    CHECK(body["messages"][1]["role"] == "user");
    CHECK(body["messages"][1]["content"] == "hi");
    CHECK(body["temperature"].get<float>() == 0.25f);
    CHECK(body["max_tokens"] == 128);
    CHECK(body["response_format"]["type"] == "json_object");

    CHECK(response.ok);
    CHECK(response.content == "hello");
    CHECK(response.reasoning == "think");
    CHECK(response.id == "gen-1");
    CHECK(response.model == "openai/gpt-4o-mini");
    CHECK(response.finish_reason == "stop");
    CHECK(response.usage.prompt_tokens == 5);
    CHECK(response.usage.completion_tokens == 2);
    CHECK(response.usage.total_tokens == 7);
    CHECK(response.http_status == 200);
    CHECK(response.error.empty());
}

void optional_fields() {
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");
    OpenRouterClient client(config(), fake_transport);
    ChatRequest request = simple_request();
    request.temperature = -1.0f;
    request.max_tokens = 0;
    request.json_mode = false;

    const ChatResponse response = client.chat(request);
    CHECK(response.ok);
    CHECK(response.usage.prompt_tokens == 0);
    CHECK(response.usage.completion_tokens == 0);
    CHECK(response.usage.total_tokens == 0);
    const nlohmann::json body = nlohmann::json::parse(g_fake.request.body);
    CHECK(!body.contains("temperature"));
    CHECK(!body.contains("max_tokens"));
    CHECK(!body.contains("response_format"));
}

void trailing_slash() {
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");
    OpenRouterConfig value = config();
    value.base_url = "https://example.test/api/v1///";
    OpenRouterClient client(value, fake_transport);
    CHECK(client.chat(simple_request()).ok);
    CHECK(g_fake.request.url == "https://example.test/api/v1/chat/completions");
}

void attribution_headers() {
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");
    OpenRouterClient plain(config(), fake_transport);
    CHECK(plain.chat(simple_request()).ok);
    CHECK(!has_header(g_fake.request, "HTTP-Referer"));
    CHECK(!has_header(g_fake.request, "X-OpenRouter-Title"));

    OpenRouterConfig value = config();
    value.app_url = "https://zillions.example";
    value.app_name = "Zillions";
    OpenRouterClient attributed(value, fake_transport);
    CHECK(attributed.chat(simple_request()).ok);
    CHECK(header(g_fake.request, "HTTP-Referer") == "https://zillions.example");
    CHECK(header(g_fake.request, "X-OpenRouter-Title") == "Zillions");
}

void api_error() {
    fake_response(401, R"({"error":{"message":"No auth credentials found","code":401}})");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "HTTP 401: No auth credentials found");
    CHECK(response.error_code == 401);
    CHECK(response.http_status == 401);
    CHECK(response.raw == R"({"error":{"message":"No auth credentials found","code":401}})");
}

void non_json_error() {
    fake_response(500, "upstream exploded");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "HTTP 500");
    CHECK(response.http_status == 500);
    CHECK(response.raw == "upstream exploded");
}

void malformed_success() {
    fake_response(200, "{not json");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "invalid JSON response");
    CHECK(response.raw == "{not json");
}

void empty_choices() {
    fake_response(200, R"({"id":"gen-1","choices":[]})");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "response has no choices");
}

void invalid_choice() {
    fake_response(200, R"({"choices":[null]})");
    OpenRouterClient client(config(), fake_transport);
    ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "invalid choice");

    fake_response(200, R"({"choices":[{}]})");
    response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "invalid choice");
}

void plaintext_base_url() {
    g_fake = FakeState{};
    OpenRouterConfig value = config();
    value.base_url = "http://openrouter.ai/api/v1";
    OpenRouterClient insecure(value, fake_transport);
    const ChatResponse response = insecure.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "base_url must use https");
    CHECK(!g_fake.called);
}

void transport_error() {
    fake_response(0, "", "connection refused");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "transport: connection refused");
    CHECK(response.http_status == 0);
}

void missing_key() {
    const std::string saved = get_env("OPENROUTER_API_KEY");
    set_env("OPENROUTER_API_KEY", "");
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");

    OpenRouterClient client(config(""), fake_transport);
    CHECK(!client.has_api_key());
    const ChatResponse response = client.chat(simple_request());
    CHECK(!response.ok);
    CHECK(response.error == "OPENROUTER_API_KEY is not set");
    CHECK(!g_fake.called);

    OpenRouterClient keyed(config(), fake_transport);
    ChatRequest no_model = simple_request();
    no_model.model.clear();
    CHECK(keyed.chat(no_model).error == "model is required");
    ChatRequest no_messages = simple_request();
    no_messages.messages.clear();
    CHECK(keyed.chat(no_messages).error == "messages are empty");
    CHECK(!g_fake.called);

    set_env("OPENROUTER_API_KEY", saved);
}

void environment_key() {
    const std::string saved = get_env("OPENROUTER_API_KEY");
    set_env("OPENROUTER_API_KEY", "  env-key \n");
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");

    OpenRouterClient client(OpenRouterConfig{}, fake_transport);
    CHECK(client.has_api_key());
    CHECK(client.config().api_key == "env-key");
    CHECK(client.chat(simple_request()).ok);
    CHECK(header(g_fake.request, "Authorization") == "Bearer env-key");

    set_env("OPENROUTER_API_KEY", saved);
}

void content_parts() {
    fake_response(200, R"({"choices":[{"message":{"content":[)"
                       R"({"type":"text","text":"Hello "},)"
                       R"({"type":"text","text":"world"}]},"finish_reason":"stop"}]})");
    OpenRouterClient client(config(), fake_transport);
    const ChatResponse response = client.chat(simple_request());
    CHECK(response.ok);
    CHECK(response.content == "Hello world");
    CHECK(response.finish_reason == "stop");
}

void invalid_utf8_message() {
    fake_response(200, R"({"choices":[{"message":{"content":"ok"}}]})");
    OpenRouterClient client(config(), fake_transport);
    ChatRequest request = simple_request();
    request.messages[0].content = "bad \xFF byte";
    const ChatResponse response = client.chat(request);
    CHECK(response.ok);
    CHECK(g_fake.request.body.find("\xEF\xBF\xBD") != std::string::npos);
}

void loopback_transport() {
    const std::string saved_no_proxy = get_env("NO_PROXY");
    const std::string saved_lower_no_proxy = get_env("no_proxy");
    set_env("NO_PROXY", "127.0.0.1");
    set_env("no_proxy", "127.0.0.1");

    Listener listener = open_listener();
    std::string captured;
    std::thread server([&listener, &captured] {
        serve_once(listener, R"({"status":"ok"})", &captured);
    });

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = "http://127.0.0.1:" + std::to_string(listener.port) + "/chat/completions";
    request.headers.push_back({"X-Test", "loopback"});
    request.body = "ping-body";
    request.timeout_ms = 10000;
    const HttpResponse response = http_request(request);

    server.join();
    close_socket(listener.handle);
    set_env("NO_PROXY", saved_no_proxy);
    set_env("no_proxy", saved_lower_no_proxy);

    CHECK(response.status == 200);
    CHECK(response.body == R"({"status":"ok"})");
    CHECK(captured.find("POST /chat/completions HTTP/1.1") != std::string::npos);
    CHECK(captured.find("X-Test: loopback") != std::string::npos);
    CHECK(captured.find("Content-Length: 9") != std::string::npos);
    CHECK(captured.find("Expect: 100-continue") == std::string::npos);
    CHECK(captured.find("ping-body") != std::string::npos);
}

void loopback_get() {
    const std::string saved_no_proxy = get_env("NO_PROXY");
    const std::string saved_lower_no_proxy = get_env("no_proxy");
    set_env("NO_PROXY", "127.0.0.1");
    set_env("no_proxy", "127.0.0.1");

    Listener listener = open_listener();
    std::string captured;
    std::thread server([&listener, &captured] {
        serve_once(listener, R"({"status":"ok"})", &captured);
    });

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.url = "http://127.0.0.1:" + std::to_string(listener.port) + "/models";
    request.timeout_ms = 10000;
    const HttpResponse response = http_request(request);

    server.join();
    close_socket(listener.handle);
    set_env("NO_PROXY", saved_no_proxy);
    set_env("no_proxy", saved_lower_no_proxy);

    CHECK(response.status == 200);
    CHECK(captured.find("GET /models HTTP/1.1") != std::string::npos);
}

void invalid_header() {
    HttpRequest request;
    request.url = "http://127.0.0.1:1/";
    request.headers.push_back({"X-Test", "value\r\nInjected: yes"});
    const HttpResponse response = http_request(request);
    CHECK(response.status == 0);
    CHECK(response.error == "invalid HTTP header");
}

void live_chat() {
    OpenRouterConfig value;
    value.app_name = "Zillions";
    OpenRouterClient client(value);
    if (!client.has_api_key()) {
        std::puts("live: OPENROUTER_API_KEY is not set");
        std::exit(2);
    }

    ChatRequest request;
    const std::string model = get_env("ZL_OPENROUTER_MODEL");
    request.model = model.empty() ? "liquid/lfm-3-1.7b" : model;
    request.messages.push_back({ChatRole::User, "Reply with exactly: pong"});
    request.max_tokens = 128;
    request.temperature = 0.0f;

    const ChatResponse response = client.chat(request);
    if (!response.ok) {
        std::printf("live: failed: %s (http %d, code %d)\n", response.error.c_str(),
                    response.http_status, response.error_code);
        std::exit(1);
    }
    std::printf("live: model=%s content=%s finish=%s reasoning_bytes=%zu usage=%d/%d/%d\n",
                response.model.c_str(), response.content.c_str(),
                response.finish_reason.c_str(), response.reasoning.size(),
                response.usage.prompt_tokens, response.usage.completion_tokens,
                response.usage.total_tokens);
    if (response.content.empty()) std::printf("live: raw=%s\n", response.raw.c_str());
    CHECK(!response.content.empty());
}

void loopback_client() {
    const std::string saved_no_proxy = get_env("NO_PROXY");
    const std::string saved_lower_no_proxy = get_env("no_proxy");
    set_env("NO_PROXY", "127.0.0.1");
    set_env("no_proxy", "127.0.0.1");

    Listener listener = open_listener();
    std::string captured;
    std::thread server([&listener, &captured] {
        serve_once(listener,
                   R"({"id":"gen-2","model":"openai/gpt-4o-mini",)"
                   R"("choices":[{"message":{"content":"from server"},"finish_reason":"stop"}],)"
                   R"("usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}})",
                   &captured);
    });

    OpenRouterConfig value;
    value.api_key = "loopback-key";
    value.base_url = "http://127.0.0.1:" + std::to_string(listener.port);
    OpenRouterClient client(value);
    const ChatResponse response = client.chat(simple_request());

    server.join();
    close_socket(listener.handle);
    set_env("NO_PROXY", saved_no_proxy);
    set_env("no_proxy", saved_lower_no_proxy);

    CHECK(response.ok);
    CHECK(response.id == "gen-2");
    CHECK(response.content == "from server");
    CHECK(response.usage.total_tokens == 5);
    CHECK(captured.find("POST /chat/completions HTTP/1.1") != std::string::npos);
    CHECK(captured.find("Authorization: Bearer loopback-key") != std::string::npos);
    CHECK(captured.find("\"model\":\"openai/gpt-4o-mini\"") != std::string::npos);
}

void loopback_response_limit() {
    const std::string saved_no_proxy = get_env("NO_PROXY");
    const std::string saved_lower_no_proxy = get_env("no_proxy");
    set_env("NO_PROXY", "127.0.0.1");
    set_env("no_proxy", "127.0.0.1");

    Listener listener = open_listener();
    std::string captured;
    std::thread server([&listener, &captured] {
        serve_once(listener, std::string(4096, 'x'), &captured);
    });

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = "http://127.0.0.1:" + std::to_string(listener.port) + "/chat/completions";
    request.body = "ping-body";
    request.timeout_ms = 10000;
    request.max_response_bytes = 64;
    const HttpResponse response = http_request(request);

    server.join();
    close_socket(listener.handle);
    set_env("NO_PROXY", saved_no_proxy);
    set_env("no_proxy", saved_lower_no_proxy);

    CHECK(response.status == 0);
    CHECK(response.error.find("exceeds") != std::string::npos);
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (!get_env("ZL_LIVE_OPENROUTER").empty()) {
        live_chat();
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    success();
    optional_fields();
    trailing_slash();
    attribution_headers();
    api_error();
    non_json_error();
    malformed_success();
    empty_choices();
    invalid_choice();
    plaintext_base_url();
    transport_error();
    missing_key();
    environment_key();
    content_parts();
    invalid_utf8_message();
    loopback_transport();
    loopback_get();
    loopback_client();
    loopback_response_limit();
    invalid_header();

#ifdef _WIN32
    WSACleanup();
#endif
    std::puts("OpenRouter: request building, response parsing, errors and loopback transport passed");
}
