// Stremio account library, displayed using shared media cards.

#pragma once

#include <memory>
#include <unordered_set>

#include <view/auto_tab_frame.hpp>

class RecyclingGrid;

class WatchlistTab : public AttachedView {
public:
    WatchlistTab();

    void onCreate() override;

    brls::View* getDefaultFocus() override;

    static brls::View* create();

private:
    BRLS_BIND(RecyclingGrid, recycler, "watchlist/grid");

    /// Resets pagination and reloads the grid; `reloadGuids` also
    /// refreshes the library guid cache (initial load and "refresh"
    /// action — not a mere sort change).
    void refresh(bool reloadGuids);
    void doRequest();

    /// a DataSource has been set for the load in progress (pages whose
    /// items are all filtered out client-side do not set one)
    bool loaded = false;

    size_t startIndex = 0;
    size_t pageSize = 60;
};
