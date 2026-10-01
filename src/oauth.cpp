#include "machinebridge/oauth.hpp"
#include "machinebridge/crypto.hpp"
#include <sstream>

namespace machinebridge {

namespace {

std::string escape_html(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#039;"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

OAuthStore::OAuthStore(std::chrono::milliseconds ttl)
    : m_ttl(ttl)
{}

std::string OAuthStore::create_code(
    const std::optional<std::string>& client_id,
    const std::string& redirect_uri,
    const std::optional<std::string>& state,
    const std::optional<std::string>& code_challenge,
    const std::string& code_challenge_method
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    clean_expired();

    std::string code = create_nonce(); // 24 cryptographically random bytes as URL safe code
    AuthCodeData data;
    data.code = code;
    data.client_id = client_id;
    data.redirect_uri = redirect_uri;
    data.state = state;
    data.code_challenge = code_challenge;
    data.code_challenge_method = code_challenge_method;
    data.created_at = std::chrono::system_clock::now();

    m_codes[code] = std::move(data);
    return code;
}

std::optional<AuthCodeData> OAuthStore::consume_code(const std::string& code) {
    std::lock_guard<std::mutex> lock(m_mutex);
    clean_expired();

    auto it = m_codes.find(code);
    if (it == m_codes.end()) return std::nullopt;

    AuthCodeData data = it->second;
    m_codes.erase(it);
    return data;
}

void OAuthStore::clean_expired() {
    auto now = std::chrono::system_clock::now();
    for (auto it = m_codes.begin(); it != m_codes.end();) {
        if (now - it->second.created_at > m_ttl) {
            it = m_codes.erase(it);
        } else {
            ++it;
        }
    }
}

nlohmann::json get_protected_resource_metadata(const std::string& base_url) {
    return nlohmann::json{
        {"resource", base_url + "/sse"},
        {"authorization_servers", nlohmann::json::array({base_url})},
        {"scopes_supported", nlohmann::json::array({"mcp"})},
        {"bearer_methods_supported", nlohmann::json::array({"header"})}
    };
}

nlohmann::json get_authorization_server_metadata(const std::string& base_url) {
    return nlohmann::json{
        {"issuer", base_url},
        {"authorization_endpoint", base_url + "/oauth/authorize"},
        {"token_endpoint", base_url + "/oauth/token"},
        {"registration_endpoint", base_url + "/oauth/register"},
        {"userinfo_endpoint", base_url + "/oauth/userinfo"},
        {"response_types_supported", nlohmann::json::array({"code"})},
        {"grant_types_supported", nlohmann::json::array({"authorization_code"})},
        {"code_challenge_methods_supported", nlohmann::json::array({"S256", "plain"})},
        {"token_endpoint_auth_methods_supported", nlohmann::json::array({"client_secret_basic", "client_secret_post", "none"})},
        {"authorization_response_iss_parameter_supported", true},
        {"scopes_supported", nlohmann::json::array({"mcp", "openid", "profile", "email"})}
    };
}

std::string render_authorize_html(const AuthorizeHtmlParams& params) {
    std::string error_alert;
    if (params.error && !params.error->empty()) {
        error_alert = "<div class=\"error-banner\">" + escape_html(*params.error) + "</div>";
    }

    std::ostringstream oss;
    oss << R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Authorize MachineBridge MCP</title>
  <style>
    :root {
      --bg: #0d1117;
      --card-bg: #161b22;
      --border: #30363d;
      --text: #c9d1d9;
      --text-heading: #f0f6fc;
      --accent: #238636;
      --accent-hover: #2ea043;
      --danger-bg: #490202;
      --danger-border: #f85149;
      --danger-text: #ff7b72;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 1.5rem;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 2rem;
      width: 100%;
      max-width: 440px;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
    }
    .badge {
      display: inline-block;
      background: #1f6feb22;
      border: 1px solid #1f6feb;
      color: #58a6ff;
      font-size: 0.75rem;
      font-weight: 600;
      padding: 0.2rem 0.6rem;
      border-radius: 20px;
      margin-bottom: 1rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }
    h1 {
      color: var(--text-heading);
      font-size: 1.5rem;
      font-weight: 600;
      margin-bottom: 0.5rem;
    }
    p.desc {
      font-size: 0.9rem;
      line-height: 1.5;
      color: #8b949e;
      margin-bottom: 1.5rem;
    }
    .error-banner {
      background: var(--danger-bg);
      border: 1px solid var(--danger-border);
      color: var(--danger-text);
      padding: 0.75rem 1rem;
      border-radius: 6px;
      font-size: 0.85rem;
      margin-bottom: 1.25rem;
    }
    .scope-box {
      background: #090d13;
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 0.75rem 1rem;
      margin-bottom: 1.5rem;
    }
    .scope-box label {
      font-size: 0.75rem;
      color: #8b949e;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      font-weight: 600;
      display: block;
      margin-bottom: 0.25rem;
    }
    .scope-box span {
      color: #58a6ff;
      font-family: monospace;
      font-size: 0.85rem;
    }
    .form-group {
      margin-bottom: 1.25rem;
    }
    label {
      display: block;
      font-size: 0.85rem;
      font-weight: 500;
      margin-bottom: 0.5rem;
      color: var(--text-heading);
    }
    input[type="password"], input[type="text"] {
      width: 100%;
      background: #0d1117;
      border: 1px solid var(--border);
      color: var(--text-heading);
      padding: 0.75rem 1rem;
      border-radius: 6px;
      font-size: 0.95rem;
      outline: none;
      transition: border-color 0.2s;
    }
    input[type="password"]:focus, input[type="text"]:focus {
      border-color: #58a6ff;
      box-shadow: 0 0 0 3px rgba(88, 166, 255, 0.2);
    }
    button {
      width: 100%;
      background: var(--accent);
      color: #fff;
      font-size: 0.95rem;
      font-weight: 600;
      padding: 0.75rem 1rem;
      border-radius: 6px;
      border: none;
      cursor: pointer;
      transition: background 0.2s;
    }
    button:hover {
      background: var(--accent-hover);
    }
    .client-info {
      font-size: 0.8rem;
      color: #8b949e;
      text-align: center;
      margin-top: 1.25rem;
    }
  </style>
</head>
<body>
  <div class="card">
    <div class="badge">Model Context Protocol</div>
    <h1>Authorize Connector</h1>
    <p class="desc">An AI client (such as ChatGPT) is requesting access to execute tools on your machine via MachineBridge.</p>
    )" << error_alert << R"(
    <form method="POST" action="/oauth/authorize">
      <input type="hidden" name="client_id" value=")" << escape_html(params.client_id.value_or("")) << R"(">
      <input type="hidden" name="redirect_uri" value=")" << escape_html(params.redirect_uri) << R"(">
      <input type="hidden" name="state" value=")" << escape_html(params.state.value_or("")) << R"(">
      <input type="hidden" name="code_challenge" value=")" << escape_html(params.code_challenge.value_or("")) << R"(">
      <input type="hidden" name="code_challenge_method" value=")" << escape_html(params.code_challenge_method.value_or("")) << R"(">

      <div class="scope-box">
        <label>Requested Permissions</label>
        <span>mcp (Execute local terminal & filesystem tools)</span>
      </div>

      <div class="form-group">
        <label for="apiKey">MachineBridge API Key</label>
        <input type="password" id="apiKey" name="apiKey" placeholder="Enter your MACHINEBRIDGE_API_KEY" required autofocus>
      </div>

      <button type="submit">Authorize Connection</button>
    </form>
    <div class="client-info">Client: )" << escape_html(params.client_id.value_or("ChatGPT")) << R"(</div>
  </div>
</body>
</html>)";

    return oss.str();
}

} // namespace machinebridge
