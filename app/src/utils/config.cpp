#ifdef __PS4__
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <arpa/inet.h>

extern "C" {
extern int ps4_mpv_use_precompiled_shaders;
extern int ps4_mpv_dump_shaders;
extern in_addr_t primary_dns;
extern in_addr_t secondary_dns;
}
#elif defined(GMCA_LINUX_TEST_BENCH)
#include <unistd.h>
#include <borealis/platforms/desktop/desktop_platform.hpp>

constexpr uint32_t MINIMUM_WINDOW_WIDTH = 640;
constexpr uint32_t MINIMUM_WINDOW_HEIGHT = 360;
#endif

#include <borealis.hpp>
#include <borealis/core/cache_helper.hpp>
#include <algorithm>
#include <mutex>
#include "utils/serial_writer.hpp"
#include <borealis/views/edit_text_dialog.hpp>
#include "api/backend.hpp"
#include "api/stremio/backend.hpp"
#include "api/http.hpp"
#include "utils/config.hpp"
#include "utils/theme_palette.hpp"
#include "utils/keybind.hpp"
#include "utils/misc.hpp"
#include "utils/ums.hpp"
#include "utils/thread.hpp"
#include "view/mpv_core.hpp"
#include "view/video_view.hpp"

std::unordered_map<AppConfig::Item, AppConfig::Option> AppConfig::settingMap = {
    {APP_THEME, {"app_theme", {"auto", "light", "dark"}}},
    {APP_LANG, {"app_lang", {brls::LOCALE_AUTO, brls::LOCALE_EN_US, brls::LOCALE_ZH_HANS, brls::LOCALE_ZH_HANT,
                                brls::LOCALE_JA, brls::LOCALE_Ko, brls::LOCALE_RU, brls::LOCALE_DE, brls::LOCALE_FR,
                                brls::LOCALE_ES, brls::LOCALE_PT, "cs", "uk", "tr", "vi"}}},
    {APP_UPDATE, {"app_update"}},
    {APP_UI_SCALE, {"app_ui_scale", {"544p", "720p", "900p", "1080p"}}},
    {SCROLLBAR, {"scrollbar"}},
    {AUDIO_CHANNELS, {"audio-channels", {"auto-safe", "stereo", "mono"}}},
    {KEYMAP, {"keymap", {"xbox", "ps", "keyboard"}}},
    {WINDOW_STATE, {"window_state"}},
    {TRANSCODEC, {"transcodec", {"h264", "hevc", "av1"}}},
    {FORCE_DIRECTPLAY, {"force_directplay"}},
    {PLAYER_VIDEO_QUALITY, {"player_video_quality"}},
    {FULLSCREEN, {"fullscreen"}},
    {OSD_ON_TOGGLE, {"osd_on_toggle"}},
    {TOUCH_GESTURE, {"touch_gesture"}},
    {CLIP_POINT, {"clip_point"}},
    {SYNC_SETTING, {"sync_setting"}},
    {MPV_VO, {"mpv_vo", {"gpu", "gpu-next", "mediacodec_embed"}}},
    {PLAYER_LOW_QUALITY, {"player_low_quality"}},
    {PLAYER_SUBS_FALLBACK, {"player_subs_fallback"}},
    // options/labels are built at runtime from media::subtitleLangCatalog() in the
    // settings tab (value stored as-is: "auto" | "off" | 2-letter code)
    {PLAYER_SUBTITLE_LANG, {"player_subtitle_lang"}},
    {PLAYER_AUTOPLAY_NEXT, {"player_autoplay_next"}},
    {PLAYER_INMEMORY_CACHE,
        {
            "player_inmemory_cache",
            {"0MB", "10MB", "20MB", "50MB", "100MB", "200MB", "500MB"},
            {0, 10, 20, 50, 100, 200, 500},
        }},
    {PLAYER_SPEED,
        {
            "player_speed",
            {"4x", "3x", "2x"},
            {400, 300, 200},
        }},
    {PLAYER_HWDEC, {"player_hwdec"}},
    {PLAYER_HWDEC_CUSTOM, {"player_hwdec_custom"}},
    {PLAYER_ASPECT, {"player_aspect", {"auto", "stretch", "crop", "4:3", "16:9"}}},
    {PLAYER_TV_MODE, {"player_tv_mode"}},
    {ALWAYS_ON_TOP, {"always_on_top"}},
    {SINGLE, {"single"}},
    {SHOW_FPS, {"show_fps"}},
    {SWAP_INTERVAL, {"swap_interval"}},
    {APP_SWAP_ABXY, {"app_swap_abxy"}},
    {TEXTURE_CACHE_NUM, {"texture_cache_num"}},
    {REQUEST_THREADS, {"request_threads", {"1", "2", "4", "8"}, {1, 2, 4, 8}}},
    {REQUEST_TIMEOUT,
        {"request_timeout", {"3000", "5000", "10000", "20000", "30000"}, {3000, 5000, 10000, 20000, 30000}}},
    {HTTP_PROXY_STATUS, {"http_proxy_status"}},
    {HTTP_PROXY, {"http_proxy"}},

    {LIBRARY_SORT, {"library_sort"}},
    {SIDEBAR_LAYOUT, {"sidebar_layout"}},

    {RENAME_NOTICE_SHOWN, {"rename_notice_shown"}},

    {KEY_REFRESH, {"key_refresh"}},
    {KEY_LAST, {"key_last"}},
    {KEY_NEXT, {"key_next"}},
    {KEY_VOLUME_UP, {"key_volume_up"}},
    {KEY_VOLUME_DOWN, {"key_volume_down"}},
    {KEY_VIDEO_PROFILE, {"key_video_profile"}},
    {KEY_FORWARD, {"key_forward"}},
    {KEY_REWIND, {"key_rewind"}},
    {KEY_SETTING, {"key_setting"}},
    {KEY_VIDEO_QUALITY, {"key_video_quality"}},
    {KEY_VIDEO_SPEED, {"key_video_speed"}},
    {KEY_VIDEO_OSD, {"key_video_osd"}},
    {KEY_VIDEO_PAUSE, {"key_video_pause"}},
};

static std::string generateDeviceId() {
#ifdef GMCA_LINUX_TEST_BENCH
    static const std::vector<std::string> dev_names = {
        "/sys/devices/virtual/dmi/id/board_serial",
        "/proc/device-tree/serial-number",
        "/etc/machine-id",
    };
    for (auto& path : dev_names) {
        std::ifstream f(path.c_str());
        if (f.is_open()) {
            std::string name;
            std::getline(f, name);
            if (name.size() > 0) {
                return misc::hexEncode((uint8_t*)name.data(), name.size());
            }
        }
    }
#endif
    return misc::randHex(16);
}

/// Per-platform data folder for a given application name.
/// Factored out so the migration can compute the old name's path.
static std::string dataDir(const std::string& name) {
#ifdef __PS4__
    return fmt::format("/data/{}", name);
#elif defined(GMCA_LINUX_TEST_BENCH)
    char* config_home = getenv("XDG_CONFIG_HOME");
    if (config_home) return fmt::format("{}/{}", config_home, name);
    return fmt::format("{}/.config/{}", getenv("HOME"), name);
#endif
}

/// Silent migration of the config folder inherited from a previous name
/// downloads must survive each rename. Returns true when a legacy dir was
/// actually relocated (the caller uses this to gate the one-time rebrand
/// welcome notice).
static bool migrateLegacyConfigDir(const std::string& legacy, const std::string& current) {
    if (legacy == current) return false;
#ifndef USE_BOOST_FILESYSTEM
    const fs::path from = fs::u8path(legacy), to = fs::u8path(current);
#else
    const fs::path from = legacy, to = current;
#endif
    try {
        if (fs::exists(from) && !fs::exists(to)) {
            fs::rename(from, to);
            brls::Logger::info("AppConfig: migrated config dir {} -> {}", legacy, current);
            return true;
        }
    } catch (const std::exception& ex) {
        brls::Logger::warning("AppConfig: config dir migration {} -> {} failed: {}", legacy, current, ex.what());
    }
    return false;
}

static fs::path configFsPath(const std::string& path) {
#if !defined(USE_BOOST_FILESYSTEM)
    return fs::u8path(path);
#else
    return fs::path(path);
#endif
}

static bool configPathExists(const std::string& path) {
    try {
        return fs::exists(configFsPath(path));
    } catch (...) {
        return false;
    }
}

/// Reads one complete config object. Missing files are not errors; malformed
/// JSON/schema throws so init() can fall back to the last known-good backup.
static bool readConfigObject(const std::string& path, nlohmann::json& out) {
#if !defined(USE_BOOST_FILESYSTEM)
    std::ifstream f(fs::u8path(path));
#else
    std::ifstream f(path);
#endif
    if (!f.is_open()) return false;
    nlohmann::json parsed = nlohmann::json::parse(f);
    if (!parsed.is_object()) throw std::runtime_error("config root is not an object");
    out = std::move(parsed);
    return true;
}

static bool validConfigObjectFile(const std::string& path) {
    try {
        nlohmann::json parsed;
        return readConfigObject(path, parsed);
    } catch (...) {
        return false;
    }
}

/// Commit config.json without ever truncating the last known-good file in place.
/// The previous valid primary becomes .bak; a corrupt primary never replaces an
/// existing backup. rename() keeps the final hand-off atomic on the same volume.
static bool writeConfigAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";

    try {
        fs::create_directories(configFsPath(path).parent_path());
#if !defined(USE_BOOST_FILESYSTEM)
        std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
#else
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
#endif
        if (!f.is_open()) return false;
        f.write(data.data(), (std::streamsize)data.size());
        f.flush();
        const bool writeOk = f.good();
        f.close();
        if (!writeOk || !f.good()) {
            fs::remove(configFsPath(tmp));
            return false;
        }

        const bool keepCurrent = validConfigObjectFile(path);
        if (keepCurrent) {
            if (fs::exists(configFsPath(bak))) fs::remove(configFsPath(bak));
            fs::rename(configFsPath(path), configFsPath(bak));
        } else if (fs::exists(configFsPath(path))) {
            // Do not poison a known-good .bak with a corrupt/truncated primary.
            fs::remove(configFsPath(path));
        }

        try {
            fs::rename(configFsPath(tmp), configFsPath(path));
        } catch (...) {
            if (keepCurrent && !fs::exists(configFsPath(path)) && fs::exists(configFsPath(bak)))
                fs::rename(configFsPath(bak), configFsPath(path));
            if (fs::exists(configFsPath(tmp))) fs::remove(configFsPath(tmp));
            throw;
        }
        return true;
    } catch (const std::exception& ex) {
        brls::Logger::warning("AppConfig atomic write {}: {}", path, ex.what());
        try {
            if (fs::exists(configFsPath(tmp))) fs::remove(configFsPath(tmp));
        } catch (...) {
        }
        return false;
    }
}

bool AppConfig::init() {
    // Chained most-recent-first; the !exists(to) guard means only the first
    // applicable source migrates. pleNx 0.2.0 already targets the GMCA folder
    // (BUILD_PACKAGE_NAME), so existing pleNx data is relocated here, and the
    // renamed GMCA build then reads it in place.
    // Only one source can migrate (the !exists(to) guard), so at most one of
    // these is true — enough to flag "this user just came from a legacy build".
    this->migratedFromLegacy = migrateLegacyConfigDir(dataDir("pleNx"), this->configDir());
    this->migratedFromLegacy |= migrateLegacyConfigDir(dataDir("Switchlex"), this->configDir());
    const std::string path = this->configDir() + "/config.json";
    const std::string backupPath = path + ".bak";
    this->recoveryState = RecoveryState::None;

    auto resetSerializedState = [this]() {
        // get_to() can fail part-way through a schema mismatch; reset every
        // serialized field before trying the backup or continuing with defaults.
        this->user_id.clear();
        this->device.clear();
        this->users.clear();
        this->servers.clear();
        this->setting = nlohmann::json::object();
        this->remotes.clear();
        this->pins.clear();
        this->persisted = nlohmann::json::object();
        this->server_url.clear();
        this->server_token.clear();
    };
    auto applyConfig = [this, &resetSerializedState](const nlohmann::json& parsed) {
        resetSerializedState();
        try {
            parsed.get_to(*this);
            this->persisted = parsed;
        } catch (...) {
            resetSerializedState();
            throw;
        }
    };

    const bool primaryExists = configPathExists(path);
    const bool backupExists = configPathExists(backupPath);
    bool loaded = false;
    bool primaryDamaged = false;

    if (primaryExists) {
        try {
            nlohmann::json parsed;
            if (readConfigObject(path, parsed)) {
                applyConfig(parsed);
                loaded = true;
                brls::Logger::info("Load config from: {}", path);
            }
        } catch (const std::exception& ex) {
            primaryDamaged = true;
            resetSerializedState();
            brls::Logger::error("AppConfig::load primary {}: {}", path, ex.what());
        }
    }

    if (!loaded && backupExists) {
        try {
            nlohmann::json parsed;
            if (readConfigObject(backupPath, parsed)) {
                applyConfig(parsed);
                loaded = true;
                this->recoveryState = RecoveryState::RestoredBackup;
                brls::Logger::warning("AppConfig: recovered damaged/missing config from {}", backupPath);
            }
        } catch (const std::exception& ex) {
            resetSerializedState();
            brls::Logger::error("AppConfig::load backup {}: {}", backupPath, ex.what());
        }
    }

    if (!loaded && (primaryDamaged || backupExists)) {
        // First launch (neither file exists) is normal. Any existing-but-invalid
        // state is instead repaired to defaults so the app can still start.
        resetSerializedState();
        this->recoveryState = RecoveryState::ResetDefaults;
        brls::Logger::warning("AppConfig: damaged config could not be recovered; using defaults");
    }

#if defined(GMCA_LINUX_TEST_BENCH)
    brls::DesktopPlatform::GAMEPAD_DB = configDir() + "/gamecontrollerdb.txt";
    if (this->getItem(AppConfig::SINGLE, false) && misc::sendIPC(this->ipcSocket(), "{}")) {
        brls::Logger::warning("AppConfig single instance");
        return false;
    }
    // 加载窗口位置
    auto wstate = this->getItem(AppConfig::WINDOW_STATE, std::string{""});
    if (wstate.size() > 0) {
        int hXPos, hYPos, monitor;
        uint32_t hWidth, hHeight;
        sscanf(wstate.c_str(), "%d,%ux%u,%dx%d", &monitor, &hWidth, &hHeight, &hXPos, &hYPos);
        if (hWidth > 0 && hHeight > 0) {
            VideoContext::sizeH = hHeight;
            VideoContext::sizeW = hWidth;
            VideoContext::posX = (float)hXPos;
            VideoContext::posY = (float)hYPos;
            VideoContext::monitorIndex = monitor;
        }
    }
    // 窗口将要关闭时, 保存窗口状态配置
    brls::Application::getExitEvent()->subscribe([this]() {
        if (std::isnan(VideoContext::posX) || std::isnan(VideoContext::posY)) return;
        if (VideoContext::FULLSCREEN) return;
        auto videoContext = brls::Application::getPlatform()->getVideoContext();
        uint32_t width = VideoContext::sizeW;
        uint32_t height = VideoContext::sizeH;
        if (width == 0) width = brls::Application::ORIGINAL_WINDOW_WIDTH;
        if (height == 0) height = brls::Application::ORIGINAL_WINDOW_HEIGHT;
        this->setItem(AppConfig::WINDOW_STATE, fmt::format("{},{}x{},{}x{}", videoContext->getCurrentMonitorIndex(),
                                                   width, height, (int)VideoContext::posX, (int)VideoContext::posY));
        this->save();
    });
#elif defined(__PS4__)
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0) brls::Logger::error("cannot load net module");
    primary_dns = inet_addr("223.5.5.5");
    secondary_dns = inet_addr("1.1.1.1");
    ps4_mpv_use_precompiled_shaders = 1;
    ps4_mpv_dump_shaders = 0;
    // 在加载第一帧之后隐藏启动画面
    brls::sync([]() { sceSystemServiceHideSplashScreen(); });
#endif

    std::string uiScale = this->getItem(APP_UI_SCALE, std::string(""));
    if (uiScale == "544p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 960;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 544;
    } else if (uiScale == "720p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1280;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 720;
    } else if (uiScale == "900p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1600;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 900;
    } else if (uiScale == "1080p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1920;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 1080;
    }

    AppConfig::SYNC = this->getItem(SYNC_SETTING, true);

    HTTP::TIMEOUT = this->getItem(REQUEST_TIMEOUT, 3000L);
    HTTP::PROXY_STATUS = this->getItem(HTTP_PROXY_STATUS, false);
    HTTP::PROXY = this->getItem(HTTP_PROXY, std::string("http://192.168.1.1:1080"));

    // 初始化是否全屏，必须在创建窗口前设置此值
    VideoContext::FULLSCREEN = this->getItem(FULLSCREEN, false);

    MPVCore::OSD_ON_TOGGLE = this->getItem(OSD_ON_TOGGLE, true);
    MPVCore::TOUCH_GESTURE = this->getItem(TOUCH_GESTURE, true);
    MPVCore::CLIP_POINT = this->getItem(CLIP_POINT, true);
    // 初始化内存缓存大小
    MPVCore::INMEMORY_CACHE = this->getItem(PLAYER_INMEMORY_CACHE, 10);
    // 是否使用低质量解码
#ifdef __PS4__
    MPVCore::LOW_QUALITY = this->getItem(PLAYER_LOW_QUALITY, true);
#else
    MPVCore::LOW_QUALITY = this->getItem(PLAYER_LOW_QUALITY, false);
#endif
    MPVCore::SUBS_FALLBACK = this->getItem(PLAYER_SUBS_FALLBACK, true);

    // 初始化是否使用硬件加速
    MPVCore::VO = this->getItem(MPV_VO, MPVCore::VO);
    MPVCore::HARDWARE_DEC = this->getItem(PLAYER_HWDEC, true);
    MPVCore::FORCE_DIRECTPLAY = this->getItem(FORCE_DIRECTPLAY, false);
    // play (high-bitrate / unsupported codecs -> slideshow), so default to a
    // smooth 4 Mbps H.264 transcode; "Auto" (0 = direct play) stays selectable
    // in the player quality menu. Other platforms default to direct play.
    // Persisted now (it was reset every launch, so the user's lowered choice
    // never survived a restart).
    MPVCore::VIDEO_QUALITY = this->getItem(PLAYER_VIDEO_QUALITY, (int64_t)0);
    MPVCore::VIDEO_CODEC = this->getItem(TRANSCODEC, MPVCore::VIDEO_CODEC);
    MPVCore::AUDIO_CHANNELS = this->getItem(AUDIO_CHANNELS, MPVCore::AUDIO_CHANNELS);
    // 初始化自定义的硬件加速方案
    MPVCore::PLAYER_HWDEC_METHOD = this->getItem(PLAYER_HWDEC_CUSTOM, MPVCore::PLAYER_HWDEC_METHOD);
    // 初始化默认的倍速设定
    MPVCore::VIDEO_SPEED = this->getItem(PLAYER_SPEED, MPVCore::VIDEO_SPEED);
    // 初始化视频比例
    MPVCore::VIDEO_ASPECT = this->getItem(PLAYER_ASPECT, MPVCore::VIDEO_ASPECT);
    // TV-style OSD by default: the progress bar is focusable and left/right
    // seek from it — the natural behaviour on a controller-driven device
    MPVCore::OSD_TV_MODE = this->getItem(PLAYER_TV_MODE, true);

    ThreadPool::max_thread_num = this->getItem(REQUEST_THREADS, ThreadPool::max_thread_num);

    // 初始化 deviceId
    if (this->device.empty()) this->device = generateDeviceId();

    // 初始化i18n
    brls::Platform::APP_LOCALE_DEFAULT = this->getItem(APP_LANG, brls::LOCALE_AUTO);

    brls::Application::setFPSStatus(this->getItem(SHOW_FPS, false));
    VideoContext::swapInterval = this->getItem(SWAP_INTERVAL, 1);

    // 初始化 KeyBind
    KeyBind::setLast(this->getItem(KEY_LAST, std::string{"pgup"}));
    KeyBind::setNext(this->getItem(KEY_NEXT, std::string{"pgdn"}));
    KeyBind::setVolumeUp(this->getItem(KEY_VOLUME_UP, std::string{"0"}));
    KeyBind::setVolumeDown(this->getItem(KEY_VOLUME_DOWN, std::string{"9"}));
    KeyBind::setVideoProfile(this->getItem(KEY_VIDEO_PROFILE, std::string{"f1"}));
    KeyBind::setVideoQuality(this->getItem(KEY_VIDEO_QUALITY, std::string{"f2"}));
    KeyBind::setVideoSpeed(this->getItem(KEY_VIDEO_SPEED, std::string{"f3"}));
    KeyBind::setSetting(this->getItem(KEY_SETTING, std::string{"f4"}));
    KeyBind::setRefresh(this->getItem(KEY_REFRESH, std::string{"f5"}));
    KeyBind::setForward(this->getItem(KEY_FORWARD, std::string{"]"}));
    KeyBind::setRewind(this->getItem(KEY_REWIND, std::string{"["}));
    KeyBind::setVideoOsd(this->getItem(KEY_VIDEO_OSD, std::string{"o"}));
    KeyBind::setVideoPause(this->getItem(KEY_VIDEO_PAUSE, std::string{"space"}));

    // 初始化一些在创建窗口之后才能初始化的内容
    brls::Application::getWindowCreationDoneEvent()->subscribe([this]() {
        if (this->getItem(APP_SWAP_ABXY, false))
        {
            // 对于 PS4 来说，初始化时会加载系统设置，可能在那时已经交换过按键
            // 所以这里需要读取 isSwapInputKeys 的值，而不是直接设置为 true
            brls::Application::setSwapInputKeys(!brls::Application::isSwapInputKeys());
        }

        // 初始化主题
        std::string appTheme = this->getItem(APP_THEME, std::string("auto"));
        if (appTheme == "light") {
            brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::LIGHT);
        } else if (appTheme == "dark") {
            brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
        }

        // 初始化纹理缓存数量
#ifdef __PS4__
        brls::TextureCache::instance().cache.setCapacity(1);
        brls::TextureCache::instance().cache.setByteCapacity(128 * 1024 * 1024);
#else
        brls::TextureCache::instance().cache.setCapacity(getItem(TEXTURE_CACHE_NUM, 200));
#endif

#if defined(GMCA_LINUX_TEST_BENCH)
        // 设置窗口最小尺寸
        brls::Application::getPlatform()->setWindowSizeLimits(MINIMUM_WINDOW_WIDTH, MINIMUM_WINDOW_HEIGHT, 0, 0);
        if (this->getItem(ALWAYS_ON_TOP, false)) {
            brls::Application::getPlatform()->setWindowAlwaysOnTop(true);
        }
#endif

#if defined(GMCA_LINUX_TEST_BENCH)
        // Init the Linux test bench F11 fullscreen shortcut.
        brls::Application::getPlatform()->getInputManager()->getKeyboardKeyStateChanged()->subscribe(
            [this](brls::KeyState state) {
                if (!state.pressed) return;
                switch (state.key) {
                case brls::BRLS_KBD_KEY_F11:
                    VideoContext::FULLSCREEN = !this->getItem(AppConfig::FULLSCREEN, VideoContext::FULLSCREEN);
                    this->setItem(AppConfig::FULLSCREEN, VideoContext::FULLSCREEN);
                    brls::Application::getPlatform()->getVideoContext()->fullScreen(VideoContext::FULLSCREEN);
                    break;
                default:;
                }
            });
#endif
    });

    Ums::instance().init();

    // init custom font path
    brls::FontLoader::USER_FONT_PATH = configDir() + "/font.ttf";
    brls::FontLoader::USER_ICON_PATH = configDir() + "/icon.ttf";
    if (access(brls::FontLoader::USER_ICON_PATH.c_str(), F_OK) == -1) {
        // 自定义字体不存在，使用内置字体
#ifdef __PS4__
        brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_ps.ttf");
#else
        std::string icon = getItem(KEYMAP, std::string("xbox"));
        if (icon == "xbox") {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_xbox.ttf");
        } else if (icon == "ps") {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_ps.ttf");
        } else if (brls::Application::isSwapInputKeys()) {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_keyboard_swap.ttf");
        } else {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_keyboard.ttf");
        }
#endif
    }

    brls::FontLoader::USER_EMOJI_PATH = configDir() + "/emoji.ttf";
    if (access(brls::FontLoader::USER_EMOJI_PATH.c_str(), F_OK) == -1) {
        // 自定义emoji不存在，使用内置emoji
        brls::FontLoader::USER_EMOJI_PATH = BRLS_ASSET("font/emoji.ttf");
    }

    brls::Logger::info("init {} v{}-{} device {} from {}", AppVersion::getPlatform(), AppVersion::getVersion(),
        AppVersion::getCommit(), this->device, path);
    if (this->recoveryState != RecoveryState::None) this->save();
    return true;
}

void AppConfig::save() {
    // Snapshot and FIFO publication share one short state lock. The writer
    // never acquires it: serialization and atomic disk I/O happen off the UI.
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    nlohmann::json snapshot = this->persisted;
    snapshot.update(nlohmann::json(*this));
    const auto path = configDir() + "/config.json";
    filePersistence().submit([path, snapshot = std::move(snapshot)] {
        try {
            if (!writeConfigAtomic(path, snapshot.dump(2)))
                brls::Logger::warning("AppConfig: could not save configuration");
        } catch (const std::exception& ex) {
            brls::Logger::warning("AppConfig save: {}", ex.what());
        }
    }, path);
}

AppConfig::~AppConfig() { delete this->activeBackend; }

void AppConfig::resetBackend() {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    delete this->activeBackend;
    this->activeBackend = nullptr;
}

media::Backend& AppConfig::backend() {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    if (!this->activeBackend) this->activeBackend = new stremio::StremioBackend();
    return *this->activeBackend;
}

media::BackendType AppConfig::backendTypeFromString(const std::string& type) {
    if (type != "stremio") throw std::invalid_argument("Unsupported account type");
    return media::BackendType::Stremio;
}

std::vector<std::string> AppConfig::getStremioAddons() const {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    static const std::vector<std::string> empty;
    if (this->user == this->users.end()) return empty;
    for (auto& s : this->servers)
        if (s.id == this->user->server_id && supportedStremioAccount(s)) return s.addons;
    return empty;
}

void AppConfig::setStremioAddons(const std::vector<std::string>& addons, const std::string& expectedToken) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    if (!expectedToken.empty() && server_token != expectedToken) return;
    if (this->user == this->users.end()) return;
    for (auto& s : this->servers) {
        if (s.id != this->user->server_id || !supportedStremioAccount(s)) continue;
        if (s.addons == addons) return;  // no disk write when nothing changed
        s.addons = addons;
        this->save();
        return;
    }
}

bool AppConfig::checkLogin() {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    this->resetBackend();
    this->server_url.clear();
    this->server_token.clear();
    auto is_user = [this](const AppUser& u) { return u.id == this->user_id; };
    this->user = std::find_if(this->users.begin(), this->users.end(), is_user);
    if (this->user == this->users.end()) return false;

    const auto* selected = selectedStremioServer(this->servers, this->user->server_id);
    if (!selected) {
        this->user = this->users.end();
        return false;
    }
    this->server_url = selected->urls.front();
    this->server_token = selected->access_token;
    this->applyTheme(media::BackendType::Stremio);
    return true;
}

std::string AppConfig::configDir() { return dataDir(AppVersion::getPackageName()); }

std::string AppConfig::ipcSocket() {
    return fmt::format("{}/{}.sock", configDir(), AppVersion::getPackageName());
}

void AppConfig::checkRestart(char* argv[]) {
#if defined(GMCA_LINUX_TEST_BENCH)
    if (brls::DesktopPlatform::RESTART_APP) {
        brls::Logger::info("Restart app {}", argv[0]);
        execv(argv[0], argv);
    }
#endif
}

int AppConfig::getOptionIndex(const Item item, int default_index) const {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    auto it = settingMap.find(item);
    if (setting.contains(it->second.key)) {
        try {
            std::string value = this->setting.at(it->second.key);
            for (size_t i = 0; i < it->second.options.size(); ++i)
                if (it->second.options[i] == value) return i;
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", it->second.key, e.what());
        }
    }
    return default_index;
}

int AppConfig::getValueIndex(const Item item, int default_index) const {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    auto it = settingMap.find(item);
    if (setting.contains(it->second.key)) {
        try {
            long value = this->setting.at(it->second.key);
            for (size_t i = 0; i < it->second.values.size(); ++i)
                if (it->second.values[i] == value) return i;
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", it->second.key, e.what());
        }
    }
    return default_index;
}

bool AppConfig::addServer(const AppServer& s) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    if (!supportedStremioAccount(s)) return false;
    if (s.urls.size() > 0) {
        this->server_url = s.urls.front();
    }

    for (auto& o : this->servers) {
        if (s.id == o.id && o.type == "stremio") {
            o.type = s.type;
            o.addons = s.addons;
            if (!s.name.empty()) o.name = s.name;
            if (!s.access_token.empty()) o.access_token = s.access_token;
            this->server_token = o.access_token;
            // remove old url
            for (auto it = o.urls.begin(); it != o.urls.end(); ++it) {
                if (it->compare(this->server_url) == 0) {
                    it = o.urls.erase(it);
                    break;
                }
            }
            o.urls.insert(o.urls.begin(), this->server_url);
            this->save();
            return true;
        }
    }
    this->server_token = s.access_token;
    this->servers.push_back(s);
    this->save();
    return false;
}

void AppConfig::addUser(const AppUser& u, const std::string& url) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    const auto* server = selectedStremioServer(this->servers, u.server_id);
    if (!server) return;
    AppUser account = u;
    // A fresh authenticated account must not overwrite a retained legacy profile
    // just because the two services happened to use the same user identifier.
    while (std::any_of(this->users.begin(), this->users.end(), [this, &account](const AppUser& existing) {
        return existing.id == account.id && !selectedStremioServer(this->servers, existing.server_id);
    })) account.id = "stremio:" + account.id;
    auto is_user = [this, account](const AppUser& o) {
        return o.id == account.id && selectedStremioServer(this->servers, o.server_id);
    };
    auto it = std::find_if(this->users.begin(), this->users.end(), is_user);
    if (it != this->users.end()) {
        it->name = account.name;
        it->access_token = account.access_token;
        it->server_id = account.server_id;
        it->thumb = account.thumb;
    } else {
        it = this->users.insert(it, account);
    }
    this->server_url = url;
    this->user_id = account.id;
    this->user = it;
    // keeps the active server token in sync with the active user
    this->server_token = server->access_token;
    this->resetBackend();
    this->applyTheme(media::BackendType::Stremio);
    this->save();
}

bool AppConfig::removeServer(const std::string& id) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    for (auto it = this->servers.begin(); it != this->servers.end(); ++it) {
        if (it->id == id && supportedStremioAccount(*it)) {
            this->servers.erase(it);
            this->save();
            return this->servers.empty();
        }
    }
    return false;
}

bool AppConfig::removeUser(const std::string& id) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex);
    for (auto it = this->users.begin(); it != this->users.end(); ++it) {
        if (it->id == id && selectedStremioServer(this->servers, it->server_id)) {
            this->users.erase(it);
            this->user = std::find_if(users.begin(), users.end(), [this](const auto& profile) { return profile.id == user_id; });
            this->save();
            return true;
        }
    }
    return false;
}

void AppConfig::addColor(const brls::ThemeVariant tv, const std::string& name, NVGcolor defaultColor) {
    auto& theme = (tv == brls::ThemeVariant::LIGHT) ? brls::Theme::getLightTheme() : brls::Theme::getDarkTheme();

    if (!setting.contains(name)) {
        theme.addColor(name, defaultColor);
    } else {
        unsigned int r = 0, g = 0, b = 0;
        std::string s = setting.at(name).get<std::string>();

        std::stringstream sr{s.substr(1, 2)};
        sr >> std::hex >> r;
        std::stringstream sg{s.substr(3, 2)};
        sg >> std::hex >> g;
        std::stringstream sb{s.substr(5, 2)};
        sb >> std::hex >> b;
        theme.addColor(name, nvgRGB(r, g, b));
    }
}

void AppConfig::applyTheme(std::optional<media::BackendType> type) {
    const plenx::ThemeColors& tc = type ? plenx::backendPalette(*type) : plenx::defaultPalette();
    this->applyThemeVariant(brls::ThemeVariant::DARK, tc.dark);
    this->applyThemeVariant(brls::ThemeVariant::LIGHT, tc.light);
}

void AppConfig::applyThemeVariant(brls::ThemeVariant tv, const plenx::ThemePalette& p) {
    auto& theme = (tv == brls::ThemeVariant::LIGHT) ? brls::Theme::getLightTheme() : brls::Theme::getDarkTheme();
    const NVGcolor accent = nvgRGB(p.accent.r, p.accent.g, p.accent.b);
    const NVGcolor glow = nvgRGB(p.accentGlowTop.r, p.accentGlowTop.g, p.accentGlowTop.b);
    const NVGcolor onAccent = nvgRGB(p.onAccentText.r, p.onAccentText.g, p.onAccentText.b);
    const NVGcolor listValue = nvgRGB(p.listValue.r, p.listValue.g, p.listValue.b);

    // VARIANT tokens (the per-backend accent surface).
    theme.addColor("brls/accent", accent);
    theme.addColor("brls/highlight/color1", accent);
    theme.addColor("brls/highlight/color2", glow);
    theme.addColor("brls/sidebar/active_item", accent);
    theme.addColor("brls/button/primary_enabled_background", accent);
    theme.addColor("brls/button/primary_enabled_text", onAccent);
    theme.addColor("brls/button/highlight_enabled_text", accent);
    theme.addColor("brls/button/highlight_disabled_text", accent);
    theme.addColor("brls/list/listItem_value_color", listValue);
    theme.addColor("brls/slider/line_filled", accent);

    // app tokens routed through AppConfig::addColor() so a user color override in
    // the settings JSON still wins (addColor reads `setting` before this default).
    this->addColor(tv, "color/app", accent);
    this->addColor(tv, "color/focus/bg", nvgRGBA(p.accent.r, p.accent.g, p.accent.b, 115));
}

void AppConfig::initThemes() {
    // "Dark theater" identity (UI_REDESIGN.md §3): neutral dark chrome. Only the
    // STRUCTURAL tokens are set here (constant across themes). The accent surface
    // (accent, highlight, sidebar active, primary button, slider, color/app...)
    // is VARIANT: set by applyTheme() per connected backend — see the
    // applyTheme(std::nullopt) call at the end of this function and the hooks in
    // checkLogin()/addUser()/ServerList. Theme::addColor overrides the borealis
    // values (theme.cpp), no submodule patch needed.
    auto& dark = brls::Theme::getDarkTheme();
    dark.addColor("brls/background", nvgRGB(13, 14, 17));
    dark.addColor("brls/sidebar/background", nvgRGB(16, 18, 22));
    dark.addColor("brls/highlight/background", nvgRGB(30, 33, 39));
    // press pulse: near-transparent light gray (orange suggested a
    // selection, not a press)
    dark.addColor("brls/click_pulse", nvgRGBA(255, 255, 255, 24));
    // brls:Header draws a 1px line under each section title:
    // an artifact in our design (typography is enough)
    dark.addColor("brls/header/border", nvgRGBA(0, 0, 0, 0));
    // even at rectangle_width 0, the nanovg antialiasing fringe of the
    // decorative rectangle leaves a ~1px line: transparency neutralizes it
    dark.addColor("brls/header/rectangle", nvgRGBA(0, 0, 0, 0));
    // pill toast (brls::Application::notify): translucent dark surface
    // slightly above the #0D0E11 background, white text
    dark.addColor("brls/notification/background", nvgRGBA(24, 26, 31, 235));
    dark.addColor("brls/notification/text", nvgRGB(255, 255, 255));

    auto& light = brls::Theme::getLightTheme();
    light.addColor("brls/click_pulse", nvgRGBA(0, 0, 0, 20));
    light.addColor("brls/header/border", nvgRGBA(0, 0, 0, 0));
    light.addColor("brls/header/rectangle", nvgRGBA(0, 0, 0, 0));
    // pill toast: conventional dark pill, readable on a light background
    light.addColor("brls/notification/background", nvgRGBA(45, 45, 45, 230));
    light.addColor("brls/notification/text", nvgRGB(255, 255, 255));

    // color/app (the app accent token) and color/focus/bg (translucent focus
    // background for player OSD controls) are VARIANT: set by applyThemeVariant().
    // dark scrim behind elements placed over an image (bars, badges)
    this->addColor(brls::ThemeVariant::LIGHT, "color/scrim", nvgRGBA(0, 0, 0, 160));
    this->addColor(brls::ThemeVariant::DARK, "color/scrim", nvgRGBA(0, 0, 0, 160));
    // metadata pills (detail pages)
    this->addColor(brls::ThemeVariant::LIGHT, "color/pill", nvgRGBA(0, 0, 0, 18));
    this->addColor(brls::ThemeVariant::DARK, "color/pill", nvgRGBA(255, 255, 255, 22));
    // surfaces placed over the background (content cards, PIN code panel...)
    this->addColor(brls::ThemeVariant::LIGHT, "color/surface", nvgRGB(255, 255, 255));
    this->addColor(brls::ThemeVariant::DARK, "color/surface", nvgRGB(22, 24, 29));
    // banner fade towards the background (transparent -> background color)
    this->addColor(brls::ThemeVariant::LIGHT, "color/fade_0", nvgRGBA(235, 235, 235, 0));
    this->addColor(brls::ThemeVariant::LIGHT, "color/fade_1", nvgRGB(235, 235, 235));
    this->addColor(brls::ThemeVariant::DARK, "color/fade_0", nvgRGBA(13, 14, 17, 0));
    this->addColor(brls::ThemeVariant::DARK, "color/fade_1", nvgRGB(13, 14, 17));
    // 用于骨架屏背景色
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_1", nvgRGB(245, 246, 247));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_1", nvgRGB(26, 28, 33));
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_2", nvgRGB(245, 245, 245));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_2", nvgRGB(51, 53, 55));
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_3", nvgRGBA(200, 200, 200, 16));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_3", nvgRGBA(160, 160, 160, 160));
    this->addColor(brls::ThemeVariant::LIGHT, "color/danger", nvgRGB(198, 28, 28));
    this->addColor(brls::ThemeVariant::DARK, "color/danger", nvgRGBA(198, 28, 28, 180));
    this->addColor(brls::ThemeVariant::LIGHT, "color/white", nvgRGB(255, 255, 255));
    this->addColor(brls::ThemeVariant::DARK, "color/white", nvgRGBA(255, 255, 255, 180));
    // 分割线颜色
    this->addColor(brls::ThemeVariant::LIGHT, "color/line", nvgRGB(208, 208, 208));
    this->addColor(brls::ThemeVariant::DARK, "color/line", nvgRGB(100, 100, 100));
    // 深浅配色通用的灰色字体颜色
    this->addColor(brls::ThemeVariant::LIGHT, "font/grey", nvgRGB(148, 153, 160));
    this->addColor(brls::ThemeVariant::DARK, "font/grey", nvgRGB(148, 153, 160));

    // establish the neutral pleNx DEFAULT accent for all pre-connection screens;
    // checkLogin()/addUser() re-apply the connected backend's palette afterwards.
    this->applyTheme(std::nullopt);

    // Posters, readable from the couch (UI_REDESIGN.md §4).
    switch (brls::Application::ORIGINAL_WINDOW_HEIGHT) {
        case 1080:
            brls::getStyle().addMetric("app/album/height", 250);
            brls::getStyle().addMetric("app/books/height", 320);
            brls::getStyle().addMetric("app/video/height", 340);
            brls::getStyle().addMetric("app/card/poster/width", 225);
            brls::getStyle().addMetric("app/card/poster/row", 393);
            brls::getStyle().addMetric("app/card/wide/width", 410);
            brls::getStyle().addMetric("app/card/wide/row", 286);
            brls::getStyle().addMetric("app/grid/6", 7);
            brls::getStyle().addMetric("app/grid/5", 6);
            brls::getStyle().addMetric("app/grid/4", 5);
            brls::getStyle().addMetric("app/grid/3", 4);
            brls::getStyle().addMetric("app/grid/2", 3);
            break;
        case 900:
            brls::getStyle().addMetric("app/album/height", 240);
            brls::getStyle().addMetric("app/books/height", 305);
            brls::getStyle().addMetric("app/video/height", 325);
            brls::getStyle().addMetric("app/card/poster/width", 205);
            brls::getStyle().addMetric("app/card/poster/row", 363);
            brls::getStyle().addMetric("app/card/wide/width", 375);
            brls::getStyle().addMetric("app/card/wide/row", 266);
            brls::getStyle().addMetric("app/grid/6", 6);
            brls::getStyle().addMetric("app/grid/5", 5);
            brls::getStyle().addMetric("app/grid/4", 4);
            brls::getStyle().addMetric("app/grid/3", 3);
            brls::getStyle().addMetric("app/grid/2", 2);
            break;
        default:
            brls::getStyle().addMetric("app/album/height", 225);
            brls::getStyle().addMetric("app/books/height", 280);
            brls::getStyle().addMetric("app/video/height", 300);
            // row = width x image ratio + 55 of labels (shared grid layout)
            brls::getStyle().addMetric("app/card/poster/width", 185);
            brls::getStyle().addMetric("app/card/poster/row", 333);
            brls::getStyle().addMetric("app/card/wide/width", 340);
            brls::getStyle().addMetric("app/card/wide/row", 246);
            brls::getStyle().addMetric("app/grid/6", 5);
            brls::getStyle().addMetric("app/grid/5", 4);
            brls::getStyle().addMetric("app/grid/4", 4);
            brls::getStyle().addMetric("app/grid/3", 3);
            brls::getStyle().addMetric("app/grid/2", 2);
    }
    brls::getStyle().addMetric("main/content_padding_sides", 40);
    brls::getStyle().addMetric("main/content_padding_top_bottom", 30);

    // UI redesign (UI_REDESIGN.md §3.2-3.3): bare and larger section titles
    // (the decorative bar of brls::Header disappears), rounded focus halo
    // matching the cards.
    brls::getStyle().addMetric("brls/header/rectangle_width", 0);
    brls::getStyle().addMetric("brls/header/rectangle_margin", 0);
    brls::getStyle().addMetric("brls/header/font_size", 24);
    brls::getStyle().addMetric("brls/highlight/stroke_width", 4);
    // 14 (not 10): the halo extends ~5 px beyond the frame, so its arc must
    // be wider than the posters' cornerRadius 10 to hug it.
    brls::getStyle().addMetric("brls/highlight/corner_radius", 14);
}
