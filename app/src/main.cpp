#ifdef GMCA_TEST_HARNESS
#include "harness.hpp"
#endif
#include <borealis.hpp>

#include "utils/config.hpp"
#include "utils/download.hpp"
#include "utils/offline_library.hpp"
#include "utils/image_cache.hpp"
#include "utils/network_state.hpp"
#include "utils/thread.hpp"
#include "utils/serial_writer.hpp"

#include "view/svg_image.hpp"
#include "view/disclosure_cell.hpp"
#include "view/icon_button.hpp"
#include "view/context_menu.hpp"
#include "view/custom_button.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/recyling_video.hpp"
#include "view/video_progress_slider.hpp"
#include "view/gallery_view.hpp"
#include "view/search_list.hpp"
#include "view/video_view.hpp"
#include "view/selector_cell.hpp"
#include "view/button_close.hpp"
#include "view/text_box.hpp"
#include "view/mpv_core.hpp"

#include "activity/main_activity.hpp"
#include "activity/server_list.hpp"
#include "activity/hint_activity.hpp"
#include "activity/loading_activity.hpp"
#include "tab/home_tab.hpp"
#include "tab/search_tab.hpp"
#include "tab/download_tab.hpp"
#include "tab/setting_tab.hpp"
#include "tab/playlists_tab.hpp"
#include "tab/watchlist_tab.hpp"

#if defined(__SDL2__)
#include <SDL2/SDL_main.h>
#endif

using namespace brls::literals;  // for _i18n

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::DEBUG = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "gmca.log";
            FILE* logFile = std::fopen(path, "w+");
            // line-buffered: without this the last ~16 KB of logs (including
            // the line preceding a crash) stay in the buffer and die with
            // the process — unbearable for diagnosing
            if (logFile) std::setvbuf(logFile, nullptr, _IOLBF, 0);
            brls::Logger::setLogOutput(logFile);
        } else if (std::strcmp(argv[i], "-version") == 0) {
            brls::Logger::info("{} {}", AppVersion::getDeviceName(), AppVersion::getCommit());
            return 0;
        }
    }

    std::setlocale(LC_ALL, "C.UTF-8");
    // Load cookies and settings
    auto& conf = AppConfig::instance();
    if (!conf.init()) {
        return 0;
    }

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    conf.initThemes();

    // Scroll indicator (scrollbar) visibility — global, driven by config
    // ("scrollbar", default true). Off = clean captures / a quieter chrome.
    brls::ScrollingFrame::setScrollingIndicatorVisibleGlobal(conf.getItem(AppConfig::SCROLLBAR, true));

    // Screenshot/automation harness (GMCA_NAV_PIPE input hook): keep the render
    // + input loop at full speed even while unfocused, so background navigation
    // and captures stay in sync (the default 5 FPS idle throttle desyncs them).
    if (std::getenv("GMCA_NAV_PIPE")) brls::Application::setDeactivatedFPS(60);

    ImageCache::init();
    DownloadManager::instance().init();
    OfflineLibrary::instance().init();

    // Return to the home shell when closing the application.
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow(fmt::format("{} for {}", AppVersion::getPackageName(), AppVersion::getPlatform()));

    // Have the application register an action on every activity that will quit when you press BUTTON_START
    brls::Application::setGlobalQuit(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("SVGImage", SVGImage::create);
    brls::Application::registerXMLView("DisclosureCell", DisclosureCell::create);
    brls::Application::registerXMLView("IconButton", IconButton::create);
    brls::Application::registerXMLView("MenuItem", MenuItem::create);
    brls::Application::registerXMLView("CustomButton", CustomButton::create);
    brls::Application::registerXMLView("SelectorCell", SelectorCell::create);
    brls::Application::registerXMLView("TextBox", TextBox::create);
    brls::Application::registerXMLView("ButtonClose", ButtonClose::create);
    brls::Application::registerXMLView("AutoTabFrame", AutoTabFrame::create);
    brls::Application::registerXMLView("MainTabFrame", MainTabFrame::create);
    brls::Application::registerXMLView("RecyclingGrid", RecyclingGrid::create);
    brls::Application::registerXMLView("HRecyclerFrame", HRecyclerFrame::create);
    brls::Application::registerXMLView("RecylingVideo", RecylingVideo::create);
    brls::Application::registerXMLView("GalleryView", GalleryView::create);
    brls::Application::registerXMLView("SearchList", SearchList::create);
    brls::Application::registerXMLView("VideoProgressSlider", VideoProgressSlider::create);

    brls::Application::registerXMLView("HomeTab", HomeTab::create);
    brls::Application::registerXMLView("SearchTab", SearchTab::create);
    brls::Application::registerXMLView("DownloadTab", [] { return new DownloadView(); });
    brls::Application::registerXMLView("SettingTab", SettingTab::create);
    brls::Application::registerXMLView("PlaylistsTab", PlaylistsTab::create);
    brls::Application::registerXMLView("WatchlistTab", WatchlistTab::create);

    if (!brls::Application::getPlatform()->isApplicationMode()) {
        brls::Application::pushActivity(new HintActivity());
    } else {
        // checkLogin() probes the remembered URLs of the active server
        // (config.cpp:checkLogin, now raced in parallel): called here on the
        // main thread it froze the very first frame for several seconds
        // (changed network, server off...). Show the loading screen and probe
        // in the background.
        brls::Application::pushActivity(new LoadingActivity(), brls::TransitionAnimation::NONE);
        brls::Application::blockInputs();
        brls::async([]() {
            const bool logged = AppConfig::instance().checkLogin();
            brls::sync([logged]() {
                brls::Application::unblockInputs();
                brls::Application::clear();
                if (logged) {
                    brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
                } else {
                    brls::Application::pushActivity(new ServerList(), brls::TransitionAnimation::NONE);
                }
            });
        });
    }

#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
    AppVersion::checkUpdate();
#else
    std::string v = conf.getItem(AppConfig::APP_UPDATE, std::string("NaN"));
    if (AppVersion::getVersion().compare(v)) AppVersion::checkUpdate();
#endif

    // Run the app
#ifdef GMCA_TEST_HARNESS
    gmca::test::Harness harness;
#endif
    while (brls::Application::mainLoop()) {
#ifdef GMCA_TEST_HARNESS
        harness.tick();
#endif
    }

    DownloadManager::instance().shutdown();
    stremioOperations().stop();
    ThreadPool::instance().stop();
    filePersistence().flush();

    conf.checkRestart(argv);
    // Exit
    return EXIT_SUCCESS;
}
