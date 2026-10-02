#pragma once
#include "view/auto_tab_frame.hpp"
#include "api/stremio/archive.hpp"

class RecyclingGrid;

class ArchiveTab : public AttachedView {
public:
    ArchiveTab();
    ~ArchiveTab() override;
    brls::View* getDefaultFocus() override;
private:
    void request(bool reset = true, bool random = false);
    void applyOptions(const stremio::archive::Options& options);
    void poll();
    void updateStatus(const stremio::archive::Result& result);
    BRLS_BIND(RecyclingGrid, grid, "archive/grid");
    BRLS_BIND(brls::Label, status, "archive/status");
    BRLS_BIND(brls::Label, help, "archive/help");
    BRLS_BIND(brls::InputCell, search, "archive/search");
    BRLS_BIND(brls::InputCell, years, "archive/years");
    BRLS_BIND(brls::SelectorCell, type, "archive/type");
    BRLS_BIND(brls::SelectorCell, genre, "archive/genre");
    BRLS_BIND(brls::SelectorCell, country, "archive/country");
    BRLS_BIND(brls::SelectorCell, addon, "archive/addon");
    BRLS_BIND(brls::SelectorCell, service, "archive/service");
    BRLS_BIND(brls::SelectorCell, rating, "archive/rating");
    BRLS_BIND(brls::SelectorCell, views, "archive/views");
    BRLS_BIND(brls::SelectorCell, other, "archive/other");
    BRLS_BIND(brls::SelectorCell, sort, "archive/sort");
    BRLS_BIND(brls::SelectorCell, order, "archive/order");
    BRLS_BIND(brls::Button, randomButton, "archive/random");
    BRLS_BIND(brls::Button, refreshButton, "archive/refresh");
    BRLS_BIND(brls::Button, resetButton, "archive/reset");
    stremio::archive::Filter filter;
    size_t offset = 0, total = 0, generation = 0, pollTimer = 0, indexed = size_t(-1);
    int64_t refreshed = -1;
    bool loading = false, refreshing = false;
    stremio::archive::Options options;
    std::shared_ptr<const stremio::archive::Snapshot> browseSnapshot;
};
