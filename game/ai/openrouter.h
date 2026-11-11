// OpenRouter chat-completions client. No gameplay code uses it yet; it is the
// transport and JSON layer for future model-driven features.
#pragma once

#include <string>
#include <vector>

#include "core/base.h"
#include "net/http.h"

namespace zl {

enum class ChatRole { System, User, Assistant };

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string content;
};

struct OpenRouterConfig {
    // Empty: read OPENROUTER_API_KEY when the client is constructed.
    std::string api_key;
    // Must use https; plaintext is accepted only for loopback hosts.
    std::string base_url = "https://openrouter.ai/api/v1";
    // Optional attribution headers OpenRouter shows on its dashboard.
    std::string app_name;
    std::string app_url;
    i32 timeout_ms = 60000;
};

struct ChatRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    // Negative leaves the field out and uses the model's default.
    f32 temperature = -1.0f;
    // Zero leaves the field out and uses the model's default.
    i32 max_tokens = 0;
    // Asks the model for a single JSON object via response_format.
    bool json_mode = false;
};

struct ChatUsage {
    i32 prompt_tokens = 0;
    i32 completion_tokens = 0;
    i32 total_tokens = 0;
};

struct ChatResponse {
    bool ok = false;  // HTTP and JSON structure succeeded; content may still be empty
    std::string id;
    std::string model;
    std::string content;
    std::string reasoning;  // reasoning models may put their trace here instead of content
    std::string finish_reason;
    ChatUsage usage;
    i32 http_status = 0;
    i32 error_code = 0;
    std::string error;
    std::string raw;  // the response body, for logs and diagnostics
};

using HttpTransport = HttpResponse (*)(const HttpRequest&);

class OpenRouterClient {
public:
    // The default transport is http_request; tests pass a fake.
    explicit OpenRouterClient(OpenRouterConfig config = {}, HttpTransport transport = http_request);

    const OpenRouterConfig& config() const { return config_; }
    bool has_api_key() const { return !config_.api_key.empty(); }

    // Sends one non-streaming chat completion and blocks until it finishes.
    ChatResponse chat(const ChatRequest& request) const;

private:
    OpenRouterConfig config_;
    HttpTransport transport_;
};

}  // namespace zl
