#!/usr/bin/env python3
from pathlib import Path

path = Path("app/src/tab/search_tab.cpp")
text = path.read_text()


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)


replace_once(
    '#include "api/backend.hpp"\n#include <fstream>\n',
    '#include "api/backend.hpp"\n#include <algorithm>\n#include <cstdint>\n#include <fstream>\n#include <map>\n',
    "includes",
)

replace_once(
    '''    std::string path;
    std::vector<std::string> list;
};

/// Removes the last UTF-8 code point: the IME can input multi-byte
''',
    '''    std::string path;
    std::vector<std::string> list;
};

namespace {
struct SearchDebounceState {
    int frames = 0;
    uint64_t generation = 0;
};

std::map<SearchTab*, SearchDebounceState> searchDebounceStates;
std::map<SearchTab*, brls::Event<>::Subscription> searchDebounceSubscriptions;
}  // namespace

/// Removes the last UTF-8 code point: the IME can input multi-byte
''',
    "debounce state",
)

replace_once(
    '''    this->searchSuggest->registerCell("Cell", VideoCardCell::create);
}
''',
    '''    this->searchSuggest->registerCell("Cell", VideoCardCell::create);

    searchDebounceStates[this] = {};
    auto subscription = brls::Application::getRunLoopEvent()->subscribe([this]() {
        auto it = searchDebounceStates.find(this);
        if (it == searchDebounceStates.end() || it->second.frames <= 0) return;
        if (--it->second.frames > 0 || this->currentSearch.empty()) return;
        this->searchSuggest->showSkeleton();
        this->doSearch(this->currentSearch);
    });
    searchDebounceSubscriptions.emplace(this, subscription);
}
''',
    "constructor subscription",
)

replace_once(
    'SearchTab::~SearchTab() { brls::Logger::debug("SearchTab: deleted"); }',
    '''SearchTab::~SearchTab() {
    auto sub = searchDebounceSubscriptions.find(this);
    if (sub != searchDebounceSubscriptions.end()) {
        brls::Application::getRunLoopEvent()->unsubscribe(sub->second);
        searchDebounceSubscriptions.erase(sub);
    }
    searchDebounceStates.erase(this);
    brls::Logger::debug("SearchTab: deleted");
}''',
    "destructor subscription cleanup",
)

replace_once(
    '''        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            // poster grid: the suggestions are complete items
''',
    '''        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            if (!this->currentSearch.empty()) return;
            // poster grid: the suggestions are complete items
''',
    "stale suggestions success guard",
)

replace_once(
    '''        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->searchSuggest->setError(ex);
        });
}

void SearchTab::doSearch''',
    '''        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            if (!this->currentSearch.empty()) return;
            this->searchSuggest->setError(ex);
        });
}

void SearchTab::doSearch''',
    "stale suggestions error guard",
)

replace_once(
    '''void SearchTab::doSearch(const std::string& searchTerm) {
    // offline: search the local catalog (title contains, case-insensitive)
''',
    '''void SearchTab::doSearch(const std::string& searchTerm) {
    auto state = searchDebounceStates.find(this);
    const uint64_t generation = state == searchDebounceStates.end() ? 0 : state->second.generation;

    // offline: search the local catalog (title contains, case-insensitive)
''',
    "search generation snapshot",
)

replace_once(
    '''    AppConfig::instance().backend().search(searchTerm, media::MediaKind::Any, 40,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            if (r.Items.empty()) {
''',
    '''    AppConfig::instance().backend().search(searchTerm, media::MediaKind::Any, 40,
        [ASYNC_TOKEN, searchTerm, generation](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            auto state = searchDebounceStates.find(this);
            if (state == searchDebounceStates.end() || state->second.generation != generation ||
                searchTerm != this->currentSearch)
                return;
            if (r.Items.empty()) {
''',
    "stale search success guard",
)

replace_once(
    '''        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        });
}

void SearchTab::updateInput() {
''',
    '''        [ASYNC_TOKEN, searchTerm, generation](const std::string& ex) {
            ASYNC_RELEASE
            auto state = searchDebounceStates.find(this);
            if (state == searchDebounceStates.end() || state->second.generation != generation ||
                searchTerm != this->currentSearch)
                return;
            brls::Application::notify(ex);
        });
}

void SearchTab::updateInput() {
''',
    "stale search error guard",
)

replace_once(
    '''void SearchTab::updateInput() {
    auto theme = brls::Application::getTheme();
''',
    '''void SearchTab::updateInput() {
    auto& debounce = searchDebounceStates[this];
    ++debounce.generation;
    debounce.frames = 0;
    auto theme = brls::Application::getTheme();
''',
    "debounce reset",
)

replace_once(
    '''        this->suggestHeader->setTitle("main/search/results"_i18n);
        this->searchSuggest->showSkeleton();
        this->doSearch(this->currentSearch);
''',
    '''        this->suggestHeader->setTitle("main/search/results"_i18n);
        size_t fps = brls::Application::getFPS();
        if (fps == 0) fps = 60;
        debounce.frames = std::max(1, (int)(fps * 450 / 1000));
''',
    "debounced dispatch",
)

path.write_text(text)
print("Applied PS4 search debounce + stale-response guards")
