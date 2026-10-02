/*
    GMCA — "Watchlist" sidebar tab (see watchlist_tab.hpp).
*/

#include "utils/config.hpp"
#include "tab/watchlist_tab.hpp"
#include "tab/media_movie.hpp"
#include "tab/media_series.hpp"
#include "api/backend.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/video_card.hpp"
#include "view/video_source.hpp"
#include "view/auto_tab_frame.hpp"
#include "utils/image.hpp"
#include "utils/keybind.hpp"
#include "utils/network_state.hpp"

using namespace brls::literals;  // for _i18n

class WatchlistFilter : public brls::Box {
public:
    WatchlistFilter() {
        this->inflateFromXMLRes("xml/view/watchlist_filter.xml");
        brls::Logger::debug("WatchlistFilter: create");

        this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [this](...) {
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [this]() { this->event.fire(); });
            return true;
        });

        this->cancel->registerClickAction([this](...) {
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [this]() { this->event.fire(); });
            return true;
        });
        this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

        this->sortBy->setVisibility(brls::Visibility::GONE);
        this->sortOrder->setVisibility(brls::Visibility::GONE);
        this->filterProgress->init("Progress", {"main/watchlist/all"_i18n, "In progress", "Unwatched", "Watched"},
                                  selectedProgress, [](int selected) { selectedProgress = selected; });

        this->filterType->init("main/media/type"_i18n,
            {
                "main/watchlist/all"_i18n,
                "main/person/movies"_i18n,
                "main/person/shows"_i18n,
            },
            selectedType, [](int selected) { selectedType = selected; });


    }

    ~WatchlistFilter() override { brls::Logger::debug("WatchlistFilter: delete"); }

    bool isTranslucent() override { return true; }

    brls::VoidEvent* getEvent() { return &this->event; }

    /// (Session) state shared with WatchlistTab::doRequest
    inline static int selectedSort = 0;   // index into sortList
    inline static int selectedOrder = 1;  // 0 ascending, 1 descending
    inline static int selectedProgress = 0;
    inline static int selectedType = 0;   // 0 all, 1 movies, 2 shows

    /// Honored provider sort fields, aligned with the selector labels
    inline static std::string sortList[] = {
        "watchlistedAt",
        "titleSort",
        "originallyAvailableAt",
    };

private:
    BRLS_BIND(brls::Box, cancel, "filter/cancel");
    BRLS_BIND(brls::SelectorCell, sortBy, "watchlist/sort/by");
    BRLS_BIND(brls::SelectorCell, sortOrder, "watchlist/sort/order");
    BRLS_BIND(brls::SelectorCell, filterProgress, "watchlist/filter/progress");
    BRLS_BIND(brls::SelectorCell, filterType, "watchlist/filter/type");

    brls::VoidEvent event;
};

WatchlistTab::WatchlistTab() {
    brls::Logger::debug("WatchlistTab: create");
    this->inflateFromXMLRes("xml/tabs/watchlist.xml");

    this->recycler->registerCell("Cell", VideoCardCell::create);
    this->recycler->onNextPage([this]() { this->doRequest(); });
}

void WatchlistTab::onCreate() {
    // (Fully hiding the tab from the bar is a follow-up — the icon-only tabs
    // have empty labels, so AutoTabFrame::clearTab cannot target them; the
    // clean fix is to add Watchlist/Playlists dynamically in MainTabFrame
    // gated by caps, like the library tabs.)
    if (AppConfig::instance().backend().caps().listKind == media::ListKind::None) {
        this->recycler->setEmpty();
        return;
    }

    auto actionRefresh = [this](...) {
        this->recycler->registerAction("main/media/sort"_i18n, brls::BUTTON_Y, [this](...) {
        auto before = std::make_tuple(WatchlistFilter::selectedSort, WatchlistFilter::selectedOrder,
                                     WatchlistFilter::selectedType, WatchlistFilter::selectedProgress);
        auto* filter = new WatchlistFilter();
        filter->getEvent()->subscribe([this, before]() {
            auto after = std::make_tuple(WatchlistFilter::selectedSort, WatchlistFilter::selectedOrder,
                                        WatchlistFilter::selectedType, WatchlistFilter::selectedProgress);
            if (after != before) this->refresh(false);
        });
        brls::Application::pushActivity(new brls::Activity(filter));
        return true;
    });

    this->refresh(true);
        return true;
    };
    this->recycler->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, actionRefresh);
    this->registerAction(KeyBind::getRefresh(), actionRefresh);

    this->refresh(true);
}

brls::View* WatchlistTab::getDefaultFocus() { return this->recycler; }

brls::View* WatchlistTab::create() { return new WatchlistTab(); }

void WatchlistTab::refresh(bool reloadGuids) {
    // offline: the watchlist lives on discover.provider (account token) — it is
    // unreachable without a connection (SPEC §4.4)
    if (NetworkState::isOffline()) {
        this->recycler->setEmpty(
            "main/download/offline_title"_i18n, "main/download/offline_section"_i18n, "icon/ico-cloud.svg");
        return;
    }
    this->startIndex = 0;
    this->loaded = false;
    this->recycler->showSkeleton();
    this->doRequest();
}

void WatchlistTab::doRequest() {
    std::string sort = WatchlistFilter::sortList[WatchlistFilter::selectedSort];
    sort += WatchlistFilter::selectedOrder ? ":desc" : ":asc";
    auto kind = WatchlistFilter::selectedType == 1 ? media::MediaKind::Movie
              : WatchlistFilter::selectedType == 2 ? media::MediaKind::Show : media::MediaKind::Any;
    ASYNC_RETAIN
    AppConfig::instance().backend().listWatchlist(
        sort, kind, this->startIndex, this->pageSize,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            this->startIndex = r.StartIndex + this->pageSize;
            bool more = !r.Items.empty() && (long)this->startIndex < r.TotalRecordCount;

                std::vector<media::Item> items;
                if (WatchlistFilter::selectedProgress == 0) {
                    items = r.Items;
                } else {
                    int wanted = WatchlistFilter::selectedProgress;
                    for (const auto& item : r.Items) {
                        bool watched = item.played();
                        bool inProgress = !watched && item.viewOffset > 0;
                        bool unwatched = !watched && item.viewOffset <= 0;
                        bool match = (wanted == 1 && inProgress) ||
                                     (wanted == 2 && unwatched) ||
                                     (wanted == 3 && watched);
                        if (match) items.push_back(item);
                    }
                }

                if (!this->loaded) {
                    if (!items.empty()) {
                        this->loaded = true;
                        this->recycler->setDataSource(new VideoDataSource(items));
                    } else if (more) {
                        // A filtered page can legitimately be empty; continue until
                        // a match appears or account pagination is exhausted.
                        this->doRequest();
                    } else if (r.TotalRecordCount == 0 && (WatchlistFilter::selectedProgress == 0)) {
                        this->recycler->setEmpty("main/favorites/empty_title"_i18n,
                            "main/favorites/empty_sub"_i18n, "icon/ico-bookmark.svg");
                    } else {
                        this->recycler->setEmpty();
                    }
                } else if (!items.empty()) {
                    auto* ds = dynamic_cast<VideoDataSource*>(this->recycler->getDataSource());
                    if (ds) {
                        ds->appendData(items);
                        this->recycler->notifyDataChanged();
                    }
                } else if (more) {
                    this->doRequest();
                }
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            if (this->loaded) {
                brls::Application::notify(ex);
            } else {
                this->recycler->setError(ex);
            }
        });
}
