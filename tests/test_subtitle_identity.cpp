#include "api/stremio/subtitle_identity.hpp"
#include <cassert>
#include <cstdio>
int main() {
    using stremio::subtitleIdentity;
    const std::string english = "en"; // backend supplies canonical language
    const std::string duplicate = "en";
    assert(subtitleIdentity(english, "normal", "url") == subtitleIdentity(duplicate, "normal", "url"));
    assert(subtitleIdentity(english, "normal", "url") != subtitleIdentity(english, "forced", "url"));
    assert(subtitleIdentity(english, "normal", "url") != subtitleIdentity(english, "sdh", "url"));
    assert(subtitleIdentity(english, "normal", "url") != subtitleIdentity(english, "normal", "provider2"));
    std::puts("subtitle identity: PASS");
}
