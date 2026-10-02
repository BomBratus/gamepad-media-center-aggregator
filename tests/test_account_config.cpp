#include "utils/account_config.hpp"
#include <cassert>
#include <iostream>

int main() {
    using nlohmann::json;
    for (const auto& type : {"", "plex", "jellyfin", "emby", "unknown"}) {
        json record = {{"id", "legacy"}, {"name", "Old connection"},
                       {"access_token", "legacy-token"}, {"urls", {"https://example.test"}},
                       {"opaque", {{"retain", true}}}};
        if (*type) record["type"] = type;
        auto server = record.get<AppServer>();
        assert(!supportedStremioAccount(server));
        assert(json(server) == record);
    }
    json account = {{"id", "one"}, {"type", "stremio"}, {"access_token", "auth-one"},
                    {"urls", {"https://api.strem.io"}}, {"addons", {"https://addon/manifest.json"}},
                    {"custom", 42}};
    auto server = account.get<AppServer>();
    assert(supportedStremioAccount(server));
    assert(json(server)["addons"] == account["addons"]);
    assert(json(server)["custom"] == 42);
    auto second = server;
    second.id = "two";
    second.access_token = "auth-two";
    std::vector<AppServer> accounts{server, second};
    auto restored = json(accounts).get<std::vector<AppServer>>();
    assert(restored.size() == 2 && restored[1].access_token == "auth-two");
    assert(selectedStremioServer(accounts, "one"));
    auto legacy = server;
    legacy.type = "plex";
    accounts.push_back(legacy);
    assert(!selectedStremioServer(accounts, "one"));
    assert(selectedStremioServer(accounts, "two"));
    server.type.clear();
    assert(!supportedStremioAccount(server));
    server.type = "stremio";
    server.access_token.clear();
    assert(!supportedStremioAccount(server));
    server.access_token = "auth";
    server.urls.clear();
    assert(!supportedStremioAccount(server));
    json user = {{"id", "one"}, {"server_id", "one"}, {"access_token", "auth-one"},
                 {"legacy_field", "retained"}};
    auto profile = user.get<AppUser>();
    assert(json(profile)["legacy_field"] == "retained");
    std::cout << "test_account_config: OK\n";
}
