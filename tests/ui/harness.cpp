#include "harness.hpp"
#include <borealis.hpp>
#include <borealis/views/dropdown.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/progress_spinner.hpp>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include "view/recycling_grid.hpp"
#include "view/video_source.hpp"
#include "activity/player_view.hpp"
#include "view/mpv_core.hpp"
#include "utils/config.hpp"

namespace gmca::test {
using json = nlohmann::json;
namespace {
std::string env(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
std::string observedId(brls::View* view, bool labels) {
    const auto& id = view->testId();
    return !labels && id.find("://") != std::string::npos ? "route/redacted" : id;
}
std::string observedClass(brls::View* view, bool labels) {
    auto description = view->describe();
    if (!labels && description.find("://") != std::string::npos) {
        const auto suffix = description.find(" (id=");
        return description.substr(0, suffix);
    }
    return description;
}
// Keep snapshots semantic and bounded. Never export free-form input/auth fields.
json node(brls::View* view, size_t& count, bool labels) {
    json out = {{"id", observedId(view, labels)}, {"class", observedClass(view, labels)}};
    ++count;
    if (labels) {
        if (auto* label = dynamic_cast<brls::Label*>(view)) {
            const auto& text = label->testText();
            if (text.size() < 512 && text.find("://") == std::string::npos) out["text"] = text;
        }
    }
    if (auto* grid = dynamic_cast<RecyclingGrid*>(view)) out["items"] = grid->getItemCount();
    if (auto* recycler = dynamic_cast<RecyclingView*>(view)) {
        if (auto* source = dynamic_cast<VideoDataSource*>(recycler->getDataSource()); source && labels) {
            out["continue_watching"] = source->testContinueWatching();
            out["media_items"] = json::array();
            for (const auto& item : source->testItems()) {
                if (out["media_items"].size() >= 40) break;
                out["media_items"].push_back({{"id", item.ratingKey}, {"key", item.key},
                    {"type", item.type}, {"watched", item.played()}, {"position_ms", item.viewOffset},
                    {"duration_ms", item.duration}});
            }
        }
    }
    if (auto* box = dynamic_cast<brls::Box*>(view)) {
        out["children"] = json::array();
        for (auto* child : box->getChildren()) {
            if (count >= 600) { out["truncated"] = true; break; }
            if (child->getVisibility() == brls::Visibility::VISIBLE)
                out["children"].push_back(node(child, count, labels));
        }
    }
    return out;
}
void inspect(brls::View* view, json& out) {
    if (view->getVisibility() != brls::Visibility::VISIBLE) return;
    const auto description = view->describe();
    if (description.find("Skeleton") != std::string::npos) out["loading"] = true;
    const std::pair<const char*, const char*> screens[] = {
        {"HomeTab", "stremio_home"}, {"StremioCatalogs", "stremio_catalogs"},
        {"MediaMovie", "stremio_movie"}, {"MediaSeries", "stremio_series"},
        {"MediaSeason", "stremio_episodes"}, {"SearchTab", "search"},
        {"GenresTab", "genres"}, {"HubView", "catalog"}, {"MediaCollection", "catalog"}, {"SearchResult", "search_results"}};
    for (auto screen : screens)
        if (description.find(screen.first) != std::string::npos) out["view"] = screen.second;
    if (description.find("ContextMenu") != std::string::npos) out["dialog"] = "context_menu";
    if (dynamic_cast<brls::Dialog*>(view)) out["dialog"] = "dialog";
    if (dynamic_cast<brls::Dropdown*>(view)) out["dialog"] = "dropdown";
    if (auto* player = dynamic_cast<PlayerView*>(view)) {
        out["player"] = true;
        out["view"] = "player";
        out["player_item"] = player->testItem();
        auto& mpv = MPVCore::instance();
        out["playback_seconds"] = mpv.playback_time;
        out["duration_seconds"] = mpv.duration;
        out["player_stopped"] = mpv.isStopped();
    }
    if (auto* recycler = dynamic_cast<RecyclingView*>(view))
        if (recycler->testLoading()) out["loading"] = true;
    if (auto* grid = dynamic_cast<RecyclingGrid*>(view))
        if (grid->testError()) out["error"] = "request_failed";
    if (auto* label = dynamic_cast<brls::Label*>(view))
        if (label->testText() == brls::getStr("main/empty/error")) out["error"] = "request_failed";
    if (dynamic_cast<brls::ProgressSpinner*>(view)) out["loading"] = true;
    if (view->testId() == "stremio/source-picker") { out["source_picker"] = true; out["view"] = "source_picker"; }
    if (view->testId() == "stremio/resume-menu") out["dialog"] = "resume";
    if (auto* box = dynamic_cast<brls::Box*>(view))
        for (auto* child : box->getChildren()) inspect(child, out);
}
json state() {
    json out = {{"ready", true}, {"input_blocked", brls::Application::isInputBlocks()}, {"dialog", nullptr}, {"loading", brls::Application::isInputBlocks()},
                {"source_picker", false}, {"player", false}, {"view", nullptr}, {"focus", nullptr}, {"error", nullptr}};
    auto stack = brls::Application::getActivitiesStack();
    out["depth"] = stack.size();
    for (auto* activity : stack)
        if (auto* player = dynamic_cast<PlayerView*>(activity->getContentView())) inspect(player, out);
    out["build_commit"] = AppVersion::getCommit();
    if (!stack.empty()) {
        auto* root = stack.back()->getContentView();
        if (root) {
            size_t count = 0;
            // Only fixture runs export labels. Live snapshots contain IDs/classes.
            out["tree"] = node(root, count, !env("GMCA_TEST_FIXTURE_URL").empty());
            out["activity"] = observedClass(root, !env("GMCA_TEST_FIXTURE_URL").empty());
            inspect(root, out);
        }
    }
    if (auto* focus = brls::Application::getCurrentFocus()) {
        out["focus"] = json::array();
        for (auto* v = focus; v; v = v->getParent()) {
            out["focus"].push_back({{"id", observedId(v, !env("GMCA_TEST_FIXTURE_URL").empty())}, {"class", observedClass(v, !env("GMCA_TEST_FIXTURE_URL").empty())}});
        }
    }
    out["mapping_verified"] = true;
    out["controllers"] = brls::Application::getPlatform()->getInputManager()->getControllersConnectedCount();
    return out;
}
int button(const std::string& name) {
    static const std::pair<const char*, int> map[] = {
        {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B},
        {"x", SDL_CONTROLLER_BUTTON_X}, {"y", SDL_CONTROLLER_BUTTON_Y},
        {"up", SDL_CONTROLLER_BUTTON_DPAD_UP}, {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"start", SDL_CONTROLLER_BUTTON_START}, {"back", SDL_CONTROLLER_BUTTON_BACK}};
    for (auto entry : map) if (entry.first == name) return entry.second;
    throw std::runtime_error("unknown controller button");
}
}
std::string requestUrl(const std::string& url, bool write) {
    const auto base = env("GMCA_TEST_FIXTURE_URL");
    if (!base.empty()) {
        if (base.rfind("http://127.0.0.1:", 0) != 0) throw std::runtime_error("fixture must use loopback");
        const std::string api = "https://api.strem.io";
        if (url.rfind(api + "/", 0) == 0) return base + url.substr(api.size());
        // Fail closed: deterministic scenarios never contact external services.
        if (url.rfind(base + "/", 0) != 0) throw std::runtime_error("external request disabled in fixture mode");
    } else if (write && !env("GMCA_TEST_READ_ONLY").empty()) {
        // These POSTs only read account state. All datastore/account writes are blocked.
        if (url != "https://api.strem.io/api/datastoreGet" &&
            url != "https://api.strem.io/api/addonCollectionGet")
            throw std::runtime_error("account write disabled in runtime test");
    }
    return url;
}
Harness::Harness() {
    path_ = env("GMCA_TEST_SOCKET");
    if (path_.empty()) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path)) throw std::runtime_error("test socket path too long");
    std::copy(path_.begin(), path_.end(), address.sun_path);
    socket_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_ < 0 || bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        chmod(path_.c_str(), 0600) < 0 || listen(socket_, 1) < 0)
        throw std::runtime_error("cannot create private test socket");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    brls::Application::setDeactivatedFPS(60);
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = "GMCA TV test controller";
    device_ = SDL_JoystickAttachVirtualEx(&desc);
    if (device_ < 0 || !SDL_IsGameController(device_)) throw std::runtime_error("SDL virtual game controller unavailable");
    joystick_ = SDL_JoystickOpen(device_);
    if (!joystick_) throw std::runtime_error("cannot open SDL virtual controller");
    // Verify every required SDL button against Borealis's real input manager.
    // Sample before the first automation frame, so validation cannot activate UI actions.
    const std::pair<int, brls::ControllerButton> mapping[] = {
        {SDL_CONTROLLER_BUTTON_A, brls::BUTTON_A}, {SDL_CONTROLLER_BUTTON_B, brls::BUTTON_B},
        {SDL_CONTROLLER_BUTTON_X, brls::BUTTON_X}, {SDL_CONTROLLER_BUTTON_Y, brls::BUTTON_Y},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, brls::BUTTON_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, brls::BUTTON_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, brls::BUTTON_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, brls::BUTTON_RIGHT},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, brls::BUTTON_LB}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, brls::BUTTON_RB},
        {SDL_CONTROLLER_BUTTON_START, brls::BUTTON_START}, {SDL_CONTROLLER_BUTTON_BACK, brls::BUTTON_BACK}};
    for (auto entry : mapping) {
        SDL_JoystickSetVirtualButton(joystick_, entry.first, 1);
        SDL_JoystickUpdate();
        brls::ControllerState sample{};
        brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&sample);
        if (!sample.buttons[entry.second]) throw std::runtime_error("virtual controller does not reach Borealis input path");
        SDL_JoystickSetVirtualButton(joystick_, entry.first, 0);
        SDL_JoystickUpdate();
    }
    exitSubscription_ = brls::Application::getExitEvent()->subscribe([this]() { cleanup(); });
}
Harness::~Harness() {
    if (!path_.empty()) brls::Application::getExitEvent()->unsubscribe(exitSubscription_);
    cleanup();
}
void Harness::cleanup() {
    if (client_ >= 0) { close(client_); client_ = -1; }
    if (socket_ >= 0) { close(socket_); socket_ = -1; unlink(path_.c_str()); }
    if (joystick_) { SDL_JoystickClose(joystick_); joystick_ = nullptr; }
    if (device_ >= 0) { SDL_JoystickDetachVirtual(device_); device_ = -1; }
}
void Harness::tick() {
    if (socket_ < 0) return;
    if (client_ < 0) {
        client_ = accept4(socket_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        clientSince_ = std::chrono::steady_clock::now();
    }
    if (client_ < 0) return;
    char data[2048];
    auto len = recv(client_, data, sizeof(data), MSG_DONTWAIT);
    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        std::chrono::steady_clock::now() - clientSince_ < std::chrono::seconds(2)) return;
    json reply;
    try {
        if (len <= 0 || len == sizeof(data)) throw std::runtime_error("invalid command size");
        auto request = json::parse(data, data + len);
        auto cmd = request.at("command").get<std::string>();
        if (cmd == "state") reply = state();
        else if (cmd == "button") {
            if (SDL_JoystickSetVirtualButton(joystick_, button(request.at("button")), request.at("pressed").get<bool>()) < 0)
                throw std::runtime_error("SDL button failed");
            reply = {{"ok", true}};
        } else if (cmd == "axis") {
            int axis = request.at("axis"); int value = request.at("value");
            if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX || value < -32768 || value > 32767 ||
                SDL_JoystickSetVirtualAxis(joystick_, axis, value) < 0) throw std::runtime_error("invalid SDL axis");
            reply = {{"ok", true}};
        } else if (cmd == "quit") { brls::Application::quit(); reply = {{"ok", true}}; }
        else throw std::runtime_error("unknown test command");
    } catch (const std::exception&) { reply = {{"error", "invalid test command"}}; }
    auto bytes = reply.dump();
    if (bytes.size() > 180000) bytes = json({{"error", "test snapshot exceeded socket limit"}}).dump();
    send(client_, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    close(client_); client_ = -1;
}
}
