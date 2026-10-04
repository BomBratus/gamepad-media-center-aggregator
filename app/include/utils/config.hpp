#pragma once

#include <borealis/core/singleton.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/theme.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <mutex>

namespace media {
class Backend;
// opaque (scoped enums default to an int base, so this is a complete type) —
// avoids pulling api/backend.hpp into every TU that includes config.hpp.
enum class BackendType;
}  // namespace media

namespace plenx {
struct ThemePalette;
}

class AppVersion {
public:
    static std::string getVersion();
    static std::string getUpdateVersion();
    static std::string getPlatform();
    static std::string getDeviceName();
    static std::string getPackageName();
    static std::string getCommit();
    static void checkUpdate(int delay = 2000, bool showUpToDateDialog = false);

    inline static std::shared_ptr<std::atomic_bool> updating = std::make_shared<std::atomic_bool>(true);
    inline static std::string git_repo = "thcolin/gamepad-media-center-aggregator";

};

#include "utils/account_config.hpp"

class AppConfig : public brls::Singleton<AppConfig> {
    using UserIter = std::vector<AppUser>::iterator;

public:
    enum Item {
        FULLSCREEN,
        APP_THEME,
        APP_LANG,
        APP_UPDATE,
        APP_UI_SCALE,
        SCROLLBAR,  // show the scroll indicator (scrollbar); default true
        AUDIO_CHANNELS,
        KEYMAP,
        WINDOW_STATE,
        TRANSCODEC,
        FORCE_DIRECTPLAY,
        PLAYER_VIDEO_QUALITY,  // transcode bitrate cap (bps); 0 = auto/direct play
        OSD_ON_TOGGLE,
        TOUCH_GESTURE,
        CLIP_POINT,
        SYNC_SETTING,
        MPV_VO,
        PLAYER_LOW_QUALITY,
        PLAYER_INMEMORY_CACHE,
        PLAYER_SPEED,
        PLAYER_HWDEC,
        PLAYER_HWDEC_CUSTOM,
        PLAYER_ASPECT,
        PLAYER_SUBS_FALLBACK,
        PLAYER_SUBTITLE_LANG,  // preferred external-subtitle language: "auto" (= app locale), "off", or a 2-letter code
        PLAYER_AUTOPLAY_NEXT,
        PLAYER_TV_MODE,
        ALWAYS_ON_TOP,
        SINGLE,
        SHOW_FPS,
        SWAP_INTERVAL,
        APP_SWAP_ABXY,  // A-B 交换 和 X-Y 交换
        TEXTURE_CACHE_NUM,
        REQUEST_THREADS,
        REQUEST_TIMEOUT,
        HTTP_PROXY_STATUS,
        HTTP_PROXY,

        /// Library sorts/filters, persisted locally (json object
        /// {itemId: "sortBy,sortOrder,filter"} — /DisplayPreferences does not
        LIBRARY_SORT,

        /// Per-server sidebar layout: order + hidden state of the reorderable
        /// tabs (libraries + Playlists + Watchlist), keyed by the active server
        /// id (getUser().server_id). Section keys collide across servers, so
        /// this MUST stay server-scoped. JSON:
        /// { "<serverId>": { "order": [ids...], "hidden": [ids...] } }
        SIDEBAR_LAYOUT,

        /// One-time "pleNx is now GMCA" welcome notice already shown. Set the
        /// first time the notice is displayed (only to users migrated from a
        RENAME_NOTICE_SHOWN,

        KEY_REFRESH,        // 刷新快捷键
        KEY_LAST,           // 上一个Tab快捷键
        KEY_NEXT,           // 下一个Tab快捷键
        KEY_VOLUME_UP,      // 音量增大快捷键
        KEY_VOLUME_DOWN,    // 音量减小快捷键
        KEY_VIDEO_PROFILE,  // 视频详情快捷键
        KEY_FORWARD,        // 快进快捷键
        KEY_REWIND,         // 快退快捷键
        KEY_SETTING,        // 设置快捷键
        KEY_VIDEO_QUALITY,  // 视频清晰度菜单快捷键
        KEY_VIDEO_SPEED,    // 视频倍速菜单快捷键
        KEY_VIDEO_OSD,      // 切换OSD显示
        KEY_VIDEO_PAUSE,    // 视频播放暂停快捷键
    };

    enum class RecoveryState { None, RestoredBackup, ResetDefaults };
    RecoveryState getRecoveryState() const { return recoveryState; }

    AppConfig() = default;
    ~AppConfig();  // out-of-line: activeBackend is a unique_ptr to an incomplete type

    bool init();
    void initThemes();
    /// (Re)applies the accent surface for `type` onto BOTH borealis theme
    /// objects (dark + light). std::nullopt = the neutral pleNx DEFAULT theme
    /// used on pre-connection screens. Structural chrome is left untouched.
    /// Must run BEFORE the activity that will read the colors is (re)built.
    void applyTheme(std::optional<media::BackendType> type);
    void save();
    bool checkLogin();

    std::string configDir();
    std::string ipcSocket();
    void checkRestart(char* argv[]);

    template <typename T>
    T getItem(const Item item, T defaultValue) {
        std::lock_guard<std::recursive_mutex> guard(stateMutex);
        const auto& o = settingMap.at(item);
        try {
            if (!setting.contains(o.key)) return defaultValue;
            return this->setting.at(o.key).get<T>();
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", o.key, e.what());
            return defaultValue;
        }
    }

    template <typename T>
    void setItem(const Item item, T data) {
        std::lock_guard<std::recursive_mutex> guard(stateMutex);
        const auto& o = settingMap.at(item);
        this->setting[o.key] = data;
        this->save();
    }

    struct Option {
        std::string key;
        std::vector<std::string> options;
        std::vector<long> values;
    };

    int getOptionIndex(const Item item, int default_index = 0) const;
    int getValueIndex(const Item item, int default_index = 0) const;
    inline const Option& getOptions(const Item item) const { return settingMap[item]; }

    bool addServer(const AppServer& s);
    void addUser(const AppUser& u, const std::string& url);
    /// Registers a server/connection WITHOUT making it active — no change to the
    /// active url/token/profile or the backend/theme. Used to store the other
    bool removeServer(const std::string& id);
    bool removeUser(const std::string& id);
    std::string getDeviceId() { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->device; }
    std::string getUserId() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->user_id; }
    std::string getUserName() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->user->name; }
    /// Active profile (name, avatar...) — valid after init()/checkLogin().
    AppUser getUser() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->user == users.end() ? AppUser{} : *this->user; }
    std::string getToken() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->server_token; }
    std::string getAccountToken() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->user->access_token; }
    std::string getUrl() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->server_url; }
    /// Stremio only: addon transport URLs of the active server (manifest URLs).
    /// Empty for other backends / when not logged in.
    std::vector<std::string> getStremioAddons() const;
    struct StremioAccount { std::string token, userId; AppUser user; std::vector<std::string> addons; };
    StremioAccount getStremioAccount() const {
        std::lock_guard<std::recursive_mutex> guard(stateMutex);
        return {server_token, user_id, user == users.end() ? AppUser{} : *user, getStremioAddons()};
    }
    /// Stremio only: replace the active server's addon list (after an account
    /// collection re-sync) and persist. No-op when unchanged or not logged in.
    void setStremioAddons(const std::vector<std::string>& addons, const std::string& expectedToken = {});
    /// Active media backend (built lazily from the active server's type).
    /// The UI talks to this; it never formats a provider URL itself.
    media::Backend& backend();
    std::vector<AppServer> getServers() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->servers; }
    /// All known connections (one AppUser = one server+profile pair).
    std::vector<AppUser> getUsers() const { std::lock_guard<std::recursive_mutex> guard(stateMutex); return this->users; }
    /// Public so the connection switcher can tint each tile by its backend brand.
    static media::BackendType backendTypeFromString(const std::string& type);

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AppConfig, user_id, device, users, servers, setting, remotes, pins);

    inline static bool SYNC = true;

    /// True for this session only when init() relocated a legacy data dir
    /// the persistent RENAME_NOTICE_SHOWN flag it gates the one-time rebrand
    /// welcome notice so it shows exactly once, and only to migrated users.
    bool migratedFromLegacy = false;

private:
    static std::unordered_map<Item, Option> settingMap;
    RecoveryState recoveryState = RecoveryState::None;
    nlohmann::json persisted = nlohmann::json::object();

    /// (Re)builds activeBackend from the active server's type on next backend().
    void resetBackend();

    /// Writes one palette variant onto the matching borealis Theme singleton.
    void applyThemeVariant(brls::ThemeVariant tv, const plenx::ThemePalette& p);

    mutable std::recursive_mutex stateMutex;
    UserIter user;
    // owning raw pointer (forward-declared type): deleted in ~AppConfig/resetBackend
    media::Backend* activeBackend = nullptr;
    std::string user_id;
    std::string server_url;
    std::string server_token;
    std::string device;
    std::string device_name;
    std::vector<AppUser> users;
    std::vector<AppServer> servers;
    nlohmann::json remotes = nlohmann::json::array();
    nlohmann::json pins = nlohmann::json::array();
    nlohmann::json setting = {};

    void addColor(const brls::ThemeVariant tv, const std::string& name, NVGcolor defaultColor);
};
