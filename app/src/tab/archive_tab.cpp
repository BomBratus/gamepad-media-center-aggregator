#include "tab/archive_tab.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_source.hpp"
#include "view/video_card.hpp"
#include "utils/dialog.hpp"
#include <borealis/core/thread.hpp>

using namespace brls::literals;
using namespace stremio::archive;

ArchiveTab::ArchiveTab() {
    inflateFromXMLRes("xml/tabs/archive.xml");
    grid->registerCell("Cell", VideoCardCell::create);
    grid->spanCount = 4;
    grid->itemImageRatio = 1.5f;
    grid->itemExtraHeight = 55;
    grid->onNextPage([this] { if (!loading && offset < total) request(false); });
    search->init("main/archive/search"_i18n, "", [this](std::string value) { filter.search = value; request(); }, "", "", 80);
    years->init("main/archive/year"_i18n, "", [this](std::string value) {
        int64_t from = 0, to = 0;
        if (!value.empty()) {
            auto separator = value.find('-');
            auto first = value.substr(0, separator);
            auto last = separator == std::string::npos ? first : value.substr(separator + 1);
            auto valid = [](const std::string& s) { return s.size() == 4 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
            if (!valid(first) || !valid(last) || first > last) {
                Dialog::show("main/archive/year_help"_i18n);
                years->setValue(filter.yearFrom ? std::to_string(filter.yearFrom) + "-" + std::to_string(filter.yearTo) : "");
                return;
            }
            from = std::stoll(first); to = std::stoll(last);
        }
        filter.yearFrom = from; filter.yearTo = to; request();
    }, "2020 / 1990-2020", "main/archive/year_help"_i18n, 9);
    type->init("main/archive/type"_i18n, {"main/archive/all"_i18n, "main/archive/movies"_i18n, "main/archive/series"_i18n}, 0,
        [this](int i) { filter.type = i == 1 ? "movie" : i == 2 ? "series" : ""; request(); });
    rating->init("main/archive/rating"_i18n, {"main/archive/all"_i18n, "5+", "6+", "7+", "8+", "9+"}, 0,
        [this](int i) { filter.minRating = i ? i + 4 : 0; request(); });
    other->init("main/archive/other"_i18n, {"main/archive/all"_i18n, "main/archive/with_poster"_i18n, "main/archive/with_summary"_i18n}, 0,
        [this](int i) { filter.other = i; request(); });
    sort->init("main/archive/sort"_i18n, {"main/archive/release"_i18n, "main/archive/updated"_i18n,
        "main/archive/added"_i18n, "main/archive/rating"_i18n, "main/archive/views"_i18n, "main/archive/name"_i18n}, 0,
        [this](int i) {
            if (i == 4 && !options.hasViews) {
                Dialog::show("main/archive/views_unavailable"_i18n);
                sort->setSelection(static_cast<int>(filter.sort), true);
                return;
            }
            filter.sort = static_cast<Sort>(i); request();
        });
    order->init("main/archive/order"_i18n, {"main/media/descending"_i18n, "main/media/ascending"_i18n}, 0,
        [this](int i) { filter.descending = i == 0; request(); });
    randomButton->registerClickAction([this](...) { if (!loading) request(false, true); return true; });
    registerAction("main/archive/random_hint"_i18n, brls::BUTTON_Y, [this](...) {
        if (!loading) request(false, true);
        return true;
    });
    refreshButton->registerClickAction([this](...) { Cache::instance().refresh(true); request(); return true; });
    resetButton->registerClickAction([this](...) {
        filter = Filter{};
        search->setValue(""); years->setValue("");
        for (auto selector : {type, genre, country, addon, service, rating, views, other, sort, order}) selector->setSelection(0, true);
        request(); return true;
    });
    applyOptions({});
    Cache::instance().refresh();
    request();
    poll();
}

ArchiveTab::~ArchiveTab() { if (pollTimer) brls::cancelDelay(pollTimer); }
brls::View* ArchiveTab::getDefaultFocus() { return search; }

void ArchiveTab::applyOptions(const Options& value) {
    options = value;
    auto selector = [this](brls::SelectorCell* cell, const std::string& label, const std::vector<std::string>& choices, std::string Filter::* field) {
        std::vector<std::string> values = {"main/archive/all"_i18n};
        values.insert(values.end(), choices.begin(), choices.end());
        int selected = 0;
        for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == filter.*field) selected = static_cast<int>(i + 1);
        cell->getEvent()->clear();
        cell->init(label, values, selected, [this, choices, field](int i) {
            filter.*field = i > 0 && static_cast<size_t>(i) <= choices.size() ? choices[i - 1] : "";
            request();
        });
    };
    selector(genre, "main/archive/genre"_i18n, options.genres, &Filter::genre);
    selector(country, options.countries.empty() ? "main/archive/country_unavailable"_i18n : "main/archive/country"_i18n,
        options.countries, &Filter::country);
    selector(addon, "main/archive/addon"_i18n, options.addons, &Filter::addon);
    selector(service, options.services.empty() ? "main/archive/service_unavailable"_i18n : "main/archive/service"_i18n,
        options.services, &Filter::service);
    views->getEvent()->clear();
    views->init(options.hasViews ? "main/archive/views"_i18n : "main/archive/views_unavailable"_i18n,
        options.hasViews ? std::vector<std::string>{"main/archive/all"_i18n, "1,000+", "10,000+", "100,000+"} : std::vector<std::string>{"main/archive/all"_i18n},
        filter.minViews == 1000 ? 1 : filter.minViews == 10000 ? 2 : filter.minViews == 100000 ? 3 : 0,
        [this](int i) { filter.minViews = i == 1 ? 1000 : i == 2 ? 10000 : i == 3 ? 100000 : 0; request(); });
    help->setText("main/archive/help"_i18n);
}

void ArchiveTab::updateStatus(const Result& result) {
    std::string text = fmt::format(fmt::runtime("main/archive/count"_i18n), result.total, result.indexed);
    if (result.refreshing) text += "  ·  " + "main/archive/refreshing"_i18n;
    if (!result.error.empty()) text += "  ·  " + brls::getStr(result.error);
    else if (result.partial) text += "  ·  " + "main/archive/partial"_i18n;
    status->setText(text);
}

void ArchiveTab::request(bool reset, bool random) {
    if (!reset && !random && loading) return;
    if (reset) { offset = 0; ++generation; }
    const auto version = generation;
    const auto start = offset;
    loading = true;
    ASYNC_RETAIN
    Cache::instance().query(filter, start, 60, random, [ASYNC_TOKEN, version, start, random](Result result) {
        ASYNC_RELEASE
        if (version != generation) return;
        loading = false;
        if (random) {
            if (result.items.empty()) Dialog::show("main/archive/no_matches"_i18n);
            else { VideoDataSource source(result.items); source.onItemSelected(this, 0); }
            return;
        }
        indexed = result.indexed; refreshed = result.refreshed; refreshing = result.refreshing;
        total = result.total;
        // Recycler callbacks can run during reload; advance first so the last
        // page cannot be requested and appended twice.
        offset = start + result.items.size();
        updateStatus(result);
        if (start == 0) {
            applyOptions(result.options);
            if (result.items.empty()) {
                grid->setDataSource(new VideoDataSource({}));
                grid->setEmpty("main/archive/no_matches"_i18n,
                    result.refreshing ? "main/archive/refreshing"_i18n : "main/archive/empty_help"_i18n, "icon/ico-media.svg");
            } else grid->setDataSource(new VideoDataSource(result.items));
        } else if (!result.items.empty()) {
            auto* source = dynamic_cast<VideoDataSource*>(grid->getDataSource());
            if (source) { source->appendData(result.items); grid->notifyDataChanged(); }
        }
    });
}

void ArchiveTab::poll() {
    pollTimer = brls::delay(2500, [this] {
        pollTimer = 0;
        ASYNC_RETAIN
        Cache::instance().query(filter, 0, 0, false, [ASYNC_TOKEN](Result result) {
            ASYNC_RELEASE
            if (!loading) {
                if (result.indexed != indexed || result.refreshed != refreshed || (!result.refreshing && refreshing)) request();
                else {
                    // A progress-label change alone must not reset the user's
                    // grid position or discard pages while they are browsing.
                    refreshing = result.refreshing;
                    result.total = total;
                    updateStatus(result);
                }
            }
            Cache::instance().refresh(); // no-op until three days old; retries failures with backoff
            poll();
        });
    });
}
