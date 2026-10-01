#include "machinebridge/oauth.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "[test_oauth] Running OAuth tests...\n";

    machinebridge::OAuthStore store(std::chrono::seconds(5));

    // 1. Code creation & consumption
    std::string code = store.create_code(
        "client-1",
        "https://chatgpt.com/callback",
        "state-123",
        "challenge-456",
        "S256"
    );
    assert(!code.empty());

    auto consumed = store.consume_code(code);
    assert(consumed.has_value());
    assert(consumed->client_id.value_or("") == "client-1");
    assert(consumed->redirect_uri == "https://chatgpt.com/callback");
    assert(consumed->state.value_or("") == "state-123");

    // Second consumption must fail (one-time code)
    auto second = store.consume_code(code);
    assert(!second.has_value());
    std::cout << "  - Code creation and one-time consumption passed\n";

    // 2. Metadata schemas
    auto res_meta = machinebridge::get_protected_resource_metadata("http://localhost:8080");
    assert(res_meta["resource"] == "http://localhost:8080/sse");
    assert(res_meta["scopes_supported"][0] == "mcp");

    auto auth_meta = machinebridge::get_authorization_server_metadata("http://localhost:8080");
    assert(auth_meta["issuer"] == "http://localhost:8080");
    assert(auth_meta["token_endpoint"] == "http://localhost:8080/oauth/token");
    std::cout << "  - OAuth metadata schemas passed\n";

    // 3. HTML template
    machinebridge::AuthorizeHtmlParams p;
    p.client_id = "ChatGPT";
    p.redirect_uri = "https://chatgpt.com/callback";
    std::string html = machinebridge::render_authorize_html(p);
    assert(html.find("Authorize MachineBridge MCP") != std::string::npos);
    assert(html.find("https://chatgpt.com/callback") != std::string::npos);
    std::cout << "  - HTML authorization template passed\n";

    std::cout << "[test_oauth] All OAuth tests passed!\n";
    return 0;
}
