#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace machinebridge {

struct AuthCodeData {
    std::string code;
    std::optional<std::string> client_id;
    std::string redirect_uri;
    std::optional<std::string> state;
    std::optional<std::string> code_challenge;
    std::string code_challenge_method = "S256";
    std::chrono::system_clock::time_point created_at;
};

class OAuthStore {
public:
    explicit OAuthStore(std::chrono::milliseconds ttl = std::chrono::minutes(10));

    std::string create_code(
        const std::optional<std::string>& client_id,
        const std::string& redirect_uri,
        const std::optional<std::string>& state = std::nullopt,
        const std::optional<std::string>& code_challenge = std::nullopt,
        const std::string& code_challenge_method = "S256"
    );

    std::optional<AuthCodeData> consume_code(const std::string& code);
    void clean_expired();

private:
    std::chrono::milliseconds m_ttl;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, AuthCodeData> m_codes;
};

nlohmann::json get_protected_resource_metadata(const std::string& base_url);
nlohmann::json get_authorization_server_metadata(const std::string& base_url);

struct AuthorizeHtmlParams {
    std::optional<std::string> client_id;
    std::string redirect_uri;
    std::optional<std::string> state;
    std::optional<std::string> code_challenge;
    std::optional<std::string> code_challenge_method;
    std::optional<std::string> error;
};

std::string render_authorize_html(const AuthorizeHtmlParams& params);

} // namespace machinebridge
