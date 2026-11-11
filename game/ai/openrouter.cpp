#include "ai/openrouter.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace zl {
namespace {

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool starts_with_ignore_case(const std::string& value, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= value.size()) return false;
        char a = value[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool is_loopback_http_url(const std::string& url) {
    constexpr const char* prefixes[] = {"http://127.0.0.1", "http://localhost", "http://[::1]"};
    for (const char* prefix : prefixes) {
        if (!starts_with_ignore_case(url, prefix)) continue;
        const std::size_t length = std::strlen(prefix);
        if (url.size() == length || url[length] == ':' || url[length] == '/') return true;
    }
    return false;
}

std::string trim_ascii(std::string value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && is_ascii_space(value[begin])) ++begin;
    while (end > begin && is_ascii_space(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::string();
    std::string result = trim_ascii(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? trim_ascii(value) : std::string();
#endif
}

const char* role_name(ChatRole role) {
    switch (role) {
        case ChatRole::System: return "system";
        case ChatRole::User: return "user";
        case ChatRole::Assistant: return "assistant";
    }
    return "user";
}

i32 integer_field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return 0;
    return it->get<i32>();
}

std::string string_field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

}  // namespace

OpenRouterClient::OpenRouterClient(OpenRouterConfig config, HttpTransport transport)
    : config_(std::move(config)), transport_(transport != nullptr ? transport : http_request) {
    if (config_.api_key.empty()) config_.api_key = environment("OPENROUTER_API_KEY");
    if (config_.base_url.empty()) config_.base_url = "https://openrouter.ai/api/v1";
    while (!config_.base_url.empty() && config_.base_url.back() == '/') config_.base_url.pop_back();
}

ChatResponse OpenRouterClient::chat(const ChatRequest& request) const {
    ChatResponse response;

    if (config_.api_key.empty()) {
        response.error = "OPENROUTER_API_KEY is not set";
        return response;
    }
    if (starts_with_ignore_case(config_.base_url, "http://") && !is_loopback_http_url(config_.base_url)) {
        response.error = "base_url must use https";
        return response;
    }
    if (request.model.empty()) {
        response.error = "model is required";
        return response;
    }
    if (request.messages.empty()) {
        response.error = "messages are empty";
        return response;
    }

    nlohmann::json body;
    body["model"] = request.model;
    nlohmann::json messages = nlohmann::json::array();
    for (const ChatMessage& message : request.messages) {
        nlohmann::json entry;
        entry["role"] = role_name(message.role);
        entry["content"] = message.content;
        messages.push_back(std::move(entry));
    }
    body["messages"] = std::move(messages);
    if (request.temperature >= 0.0f) body["temperature"] = request.temperature;
    if (request.max_tokens > 0) body["max_tokens"] = request.max_tokens;
    if (request.json_mode) body["response_format"] = {{"type", "json_object"}};

    HttpRequest http;
    http.method = HttpMethod::Post;
    http.url = config_.base_url + "/chat/completions";
    http.timeout_ms = config_.timeout_ms;
    http.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    http.headers.push_back({"Authorization", "Bearer " + config_.api_key});
    http.headers.push_back({"Content-Type", "application/json"});
    http.headers.push_back({"Accept", "application/json"});
    if (!config_.app_url.empty()) http.headers.push_back({"HTTP-Referer", config_.app_url});
    if (!config_.app_name.empty()) http.headers.push_back({"X-OpenRouter-Title", config_.app_name});

    const HttpResponse transport_response = transport_(http);
    response.raw = transport_response.body;
    response.http_status = transport_response.status;

    if (!transport_response.error.empty()) {
        response.error = "transport: " + transport_response.error;
        return response;
    }

    if (transport_response.status < 200 || transport_response.status >= 300) {
        response.error = "HTTP " + std::to_string(transport_response.status);
        const nlohmann::json error_json =
            nlohmann::json::parse(transport_response.body, nullptr, false);
        if (!error_json.is_discarded() && error_json.is_object()) {
            const auto error = error_json.find("error");
            if (error != error_json.end() && error->is_object()) {
                const std::string message = string_field(*error, "message");
                if (!message.empty()) response.error += ": " + message;
                response.error_code = integer_field(*error, "code");
            }
        }
        return response;
    }

    const nlohmann::json json =
        nlohmann::json::parse(transport_response.body, nullptr, false);
    if (json.is_discarded()) {
        response.error = "invalid JSON response";
        return response;
    }

    const auto choices = json.find("choices");
    if (choices == json.end() || !choices->is_array() || choices->empty()) {
        response.error = "response has no choices";
        return response;
    }

    response.id = string_field(json, "id");
    response.model = string_field(json, "model");

    const nlohmann::json& choice = (*choices)[0];
    if (!choice.is_object()) {
        response.error = "invalid choice";
        return response;
    }
    response.finish_reason = string_field(choice, "finish_reason");
    const auto message = choice.find("message");
    if (message == choice.end() || !message->is_object()) {
        response.error = "invalid choice";
        return response;
    }
    const auto content = message->find("content");
    if (content != message->end()) {
        if (content->is_string()) {
            response.content = content->get<std::string>();
        } else if (content->is_array()) {
            for (const nlohmann::json& part : *content) {
                if (part.is_object()) response.content += string_field(part, "text");
            }
        }
    }
    response.reasoning = string_field(*message, "reasoning");

    const auto usage = json.find("usage");
    if (usage != json.end() && usage->is_object()) {
        response.usage.prompt_tokens = integer_field(*usage, "prompt_tokens");
        response.usage.completion_tokens = integer_field(*usage, "completion_tokens");
        response.usage.total_tokens = integer_field(*usage, "total_tokens");
    }

    response.ok = true;
    return response;
}

}  // namespace zl
