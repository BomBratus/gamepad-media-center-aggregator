/*
    GMCA — server type chooser (first step of the add-server flow).
    See server_type_choose.hpp. Each cell present()s its dedicated sign-in
    screen onto the ServerList AppletFrame content stack (View::present →
    AppletFrame::pushContentView), which keeps the footer and gives focus to the
    pushed view; B (the frame's hints/back action) pops back here.
*/

#include "tab/server_type_choose.hpp"
#ifndef GMCA_STREMIO_ONLY
#include "tab/plex_add.hpp"
#include "tab/jellyfin_add.hpp"
#endif
#include "tab/stremio_add.hpp"

using namespace brls::literals;  // for _i18n

ServerTypeChoose::ServerTypeChoose() {
    this->inflateFromXMLRes("xml/tabs/server_type_choose.xml");
    brls::Logger::debug("ServerTypeChoose: create");

#ifdef GMCA_STREMIO_ONLY
    // Keep the shared XML layout so the normal multi-backend build remains the
    // single source of truth, but remove unsupported choices from focus/layout.
    // This profile is intended for the lean PS4/Stremio build.
    this->cellPlex->setVisibility(brls::Visibility::GONE);
    this->cellJellyfin->setVisibility(brls::Visibility::GONE);
    this->cellEmby->setVisibility(brls::Visibility::GONE);
#else
    this->cellPlex->registerClickAction([](brls::View* view) {
        view->present(new PlexAdd());
        return true;
    });
    this->cellJellyfin->registerClickAction([](brls::View* view) {
        view->present(new JellyfinAdd("jellyfin"));
        return true;
    });
    this->cellEmby->registerClickAction([](brls::View* view) {
        view->present(new JellyfinAdd("emby"));
        return true;
    });
#endif
    this->cellStremio->registerClickAction([](brls::View* view) {
        view->present(new StremioAdd());
        return true;
    });
}

ServerTypeChoose::~ServerTypeChoose() { brls::Logger::debug("ServerTypeChoose: delete"); }

brls::View* ServerTypeChoose::getDefaultFocus() {
#ifdef GMCA_STREMIO_ONLY
    return this->cellStremio;
#else
    return this->cellPlex;
#endif
}
