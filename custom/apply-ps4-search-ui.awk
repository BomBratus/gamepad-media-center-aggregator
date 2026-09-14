# Build-time transform for the PS4 test package.
# The pacbrew image is intentionally minimal (no Python), so keep this POSIX awk.

function indent_guard() {
    print "            auto state = searchDebounceStates.find(this);"
    print "            if (state == searchDebounceStates.end() || state->second.generation != generation ||"
    print "                searchTerm != this->currentSearch)"
    print "                return;"
}

BEGIN {
    history = 0
    do_suggest = 0
    do_search = 0
    update_input = 0
    async_search_callback = 0
    includes_done = state_done = ctor_done = dtor_done = 0
    suggest_guards = search_callbacks = update_done = 0
}

{
    line = $0

    if (line == "#include \"api/backend.hpp\"") {
        print line
        print "#include <algorithm>"
        print "#include <cstdint>"
        includes_done++
        next
    }
    if (line == "#include <fstream>") {
        print line
        print "#include <map>"
        next
    }

    if (line == "class SearchHistory {") history = 1
    if (history && line == "};") {
        print line
        print ""
        print "namespace {"
        print "struct SearchDebounceState {"
        print "    int frames = 0;"
        print "    uint64_t generation = 0;"
        print "};"
        print ""
        print "std::map<SearchTab*, SearchDebounceState> searchDebounceStates;"
        print "std::map<SearchTab*, brls::Event<>::Subscription> searchDebounceSubscriptions;"
        print "}  // namespace"
        history = 0
        state_done++
        next
    }

    if (line == "    this->searchSuggest->registerCell(\"Cell\", VideoCardCell::create);") {
        print line
        print ""
        print "    searchDebounceStates[this] = {};"
        print "    auto subscription = brls::Application::getRunLoopEvent()->subscribe([this]() {"
        print "        auto it = searchDebounceStates.find(this);"
        print "        if (it == searchDebounceStates.end() || it->second.frames <= 0) return;"
        print "        if (--it->second.frames > 0 || this->currentSearch.empty()) return;"
        print "        this->searchSuggest->showSkeleton();"
        print "        this->doSearch(this->currentSearch);"
        print "    });"
        print "    searchDebounceSubscriptions.emplace(this, subscription);"
        ctor_done++
        next
    }

    if (line == "SearchTab::~SearchTab() { brls::Logger::debug(\"SearchTab: deleted\"); }") {
        print "SearchTab::~SearchTab() {"
        print "    auto sub = searchDebounceSubscriptions.find(this);"
        print "    if (sub != searchDebounceSubscriptions.end()) {"
        print "        brls::Application::getRunLoopEvent()->unsubscribe(sub->second);"
        print "        searchDebounceSubscriptions.erase(sub);"
        print "    }"
        print "    searchDebounceStates.erase(this);"
        print "    brls::Logger::debug(\"SearchTab: deleted\");"
        print "}"
        dtor_done++
        next
    }

    if (line == "void SearchTab::doSuggest() {") do_suggest = 1
    if (line == "void SearchTab::doSearch(const std::string& searchTerm) {") {
        do_suggest = 0
        do_search = 1
        print line
        print "    auto state = searchDebounceStates.find(this);"
        print "    const uint64_t generation = state == searchDebounceStates.end() ? 0 : state->second.generation;"
        print ""
        next
    }

    if (do_suggest && line == "            ASYNC_RELEASE") {
        print line
        print "            if (!this->currentSearch.empty()) return;"
        suggest_guards++
        next
    }

    if (do_search && line == "        [ASYNC_TOKEN](const media::Container<media::Item>& r) {") {
        print "        [ASYNC_TOKEN, searchTerm, generation](const media::Container<media::Item>& r) {"
        async_search_callback = 1
        search_callbacks++
        next
    }
    if (do_search && line == "        [ASYNC_TOKEN](const std::string& ex) {") {
        print "        [ASYNC_TOKEN, searchTerm, generation](const std::string& ex) {"
        async_search_callback = 1
        search_callbacks++
        next
    }
    if (do_search && async_search_callback && line == "            ASYNC_RELEASE") {
        print line
        indent_guard()
        async_search_callback = 0
        next
    }

    if (line == "void SearchTab::updateInput() {") {
        do_search = 0
        update_input = 1
        print line
        print "    auto& debounce = searchDebounceStates[this];"
        print "    ++debounce.generation;"
        print "    debounce.frames = 0;"
        next
    }

    if (update_input && line == "        this->suggestHeader->setTitle(\"main/search/results\"_i18n);") {
        print line
        print "        size_t fps = brls::Application::getFPS();"
        print "        if (fps == 0) fps = 60;"
        print "        debounce.frames = std::max(1, (int)(fps * 450 / 1000));"
        skip_immediate_search = 2
        update_done++
        next
    }
    if (skip_immediate_search > 0) {
        skip_immediate_search--
        next
    }

    print line
}

END {
    if (includes_done != 1 || state_done != 1 || ctor_done != 1 || dtor_done != 1 ||
        suggest_guards != 2 || search_callbacks != 2 || update_done != 1) {
        print "PS4 search transform assertion failed" > "/dev/stderr"
        print "includes=" includes_done ", state=" state_done ", ctor=" ctor_done ", dtor=" dtor_done \
              ", suggest_guards=" suggest_guards ", search_callbacks=" search_callbacks ", update=" update_done > "/dev/stderr"
        exit 2
    }
}
