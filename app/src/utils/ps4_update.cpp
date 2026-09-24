#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)

#include "utils/ps4_update.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/Sysmodule.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "api/http.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "utils/thread.hpp"

using namespace brls::literals;

namespace ps4update {
namespace {

#define GMCA_STR_IMPL(x) #x
#define GMCA_STR(x) GMCA_STR_IMPL(x)

constexpr const char* kChannel = "ps4-stremio-only";
constexpr const char* kTitleId = "GMCA00000";
constexpr const char* kManifestUrl =
    "https://github.com/BomBratus/gamepad-media-center-aggregator/releases/download/"
    "ps4-stremio-latest/ps4-update.json";
constexpr const char* kPackageUrl =
    "https://github.com/BomBratus/gamepad-media-center-aggregator/releases/download/"
    "ps4-stremio-latest/GMCA-PS4-Stremio-only.pkg";
constexpr size_t kBgftHeapSize = 1024 * 1024;

struct Manifest {
    std::string version;
    std::string pkgName;
    std::string sha256;
    std::string commit;
    std::string notes;
    int64_t size = 0;
};

struct InstallResult {
    bool queued = false;
    int32_t code = 0;
    std::string stage;
};

static void* sBgftHeap = nullptr;
static bool sBgftInitialized = false;

inline uint32_t rotr(uint32_t v, uint32_t n) {
    return (v >> n) | (v << (32 - n));
}

class Sha256 {
public:
    Sha256()
        : state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            size_t take = std::min(len, block.size() - used);
            std::memcpy(block.data() + used, data, take);
            used += take;
            data += take;
            len -= take;
            if (used == block.size()) {
                transform(block.data());
                used = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        const uint64_t bitLength = total * 8;
        block[used++] = 0x80;
        if (used > 56) {
            std::fill(block.begin() + used, block.end(), 0);
            transform(block.data());
            used = 0;
        }
        std::fill(block.begin() + used, block.begin() + 56, 0);
        for (int i = 0; i < 8; ++i)
            block[63 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
        transform(block.data());

        std::array<uint8_t, 32> out{};
        for (size_t i = 0; i < state.size(); ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
        }
        return out;
    }

private:
    void transform(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
            0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
            0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
            0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
            0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
            0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
            0xc67178f2U,
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<uint32_t, 8> state;
    std::array<uint8_t, 64> block{};
    size_t used = 0;
    uint64_t total = 0;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool validSha256(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool validVersion(const std::string& value) {
    return value.size() == 5 && std::isdigit(static_cast<unsigned char>(value[0])) &&
           std::isdigit(static_cast<unsigned char>(value[1])) && value[2] == '.' &&
           std::isdigit(static_cast<unsigned char>(value[3])) && std::isdigit(static_cast<unsigned char>(value[4]));
}

int versionNumber(const std::string& value) {
    if (!validVersion(value)) return -1;
    return (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[3] - '0') * 10 + (value[4] - '0');
}

bool safePkgName(const std::string& value) {
    if (value.size() < 5 || value.compare(value.size() - 4, 4, ".pkg") != 0) return false;
    return value.find('/') == std::string::npos && value.find('\\') == std::string::npos &&
           value.find("..") == std::string::npos;
}

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open downloaded PKG for SHA-256");

    Sha256 sha;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        std::streamsize got = in.gcount();
        if (got > 0) sha.update(buffer.data(), static_cast<size_t>(got));
    }
    if (!in.eof()) throw std::runtime_error("failed while reading downloaded PKG");

    auto digest = sha.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return out;
}

int64_t fileSize(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

void ensurePackageDir() {
    if (mkdir("/data/pkg", 0777) != 0 && errno != EEXIST)
        throw std::runtime_error(fmt::format("cannot create /data/pkg (errno {})", errno));
}

Manifest fetchManifest() {
    auto body = HTTP::get(kManifestUrl, HTTP::Timeout{10000, 5000});
    auto j = nlohmann::json::parse(body);

    if (j.value("schema", 0) != 1 || j.value("channel", std::string()) != kChannel ||
        j.value("title_id", std::string()) != kTitleId)
        throw std::runtime_error("invalid PS4 update channel");

    Manifest m;
    m.version = j.value("version", std::string());
    m.pkgName = j.value("pkg_name", std::string());
    m.sha256 = lower(j.value("sha256", std::string()));
    m.commit = j.value("commit", std::string());
    m.notes = j.value("notes", std::string());
    m.size = j.value("size", int64_t(0));

    if (!validVersion(m.version) || !safePkgName(m.pkgName) || !validSha256(m.sha256) || m.size <= 0)
        throw std::runtime_error("invalid PS4 update manifest");

    return m;
}

bool initBgft(int32_t& error) {
    if (sBgftInitialized) return true;

    // The sysmodule may already be resident because Borealis links SceBgft.
    // Do not fail solely on the load result; the service init below is authoritative.
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT);

    sBgftHeap = std::malloc(kBgftHeapSize);
    if (!sBgftHeap) {
        error = -1;
        return false;
    }
    std::memset(sBgftHeap, 0, kBgftHeapSize);

    OrbisBgftInitParams params{};
    params.heap = sBgftHeap;
    params.heapSize = kBgftHeapSize;
    error = sceBgftServiceIntInit(&params);
    if (error != 0) {
        std::free(sBgftHeap);
        sBgftHeap = nullptr;
        return false;
    }
    sBgftInitialized = true;
    return true;
}

InstallResult queueInstall(const std::string& localPath) {
    InstallResult result;

    int32_t rc = sceAppInstUtilInitialize();
    if (rc != 0) {
        result.code = rc;
        result.stage = "AppInstUtil init";
        return result;
    }

    // AppInstUtil/BGFT operate outside the application sandbox. GoldHEN exposes
    // /data to homebrew, while the system service commonly sees the same file as
    // /user/data. Try the system path first and fall back to the app-visible path.
    std::vector<std::string> candidates;
    if (localPath.rfind("/data/", 0) == 0) candidates.push_back("/user" + localPath);
    candidates.push_back(localPath);

    std::string installPath;
    char titleId[16] = {};
    int32_t isApp = 0;
    for (const auto& candidate : candidates) {
        std::memset(titleId, 0, sizeof(titleId));
        rc = sceAppInstUtilGetTitleIdFromPkg(candidate.c_str(), titleId, &isApp);
        if (rc == 0) {
            installPath = candidate;
            break;
        }
    }

    if (installPath.empty()) {
        sceAppInstUtilTerminate();
        result.code = rc;
        result.stage = "PKG validation";
        return result;
    }
    if (std::string(titleId) != kTitleId || !isApp) {
        sceAppInstUtilTerminate();
        result.code = -2;
        result.stage = "unexpected Title ID";
        return result;
    }

    int32_t slot = 0;
    rc = sceAppInstUtilGetPrimaryAppSlot(kTitleId, &slot);
    if (rc == 0) {
        int32_t prep = sceAppInstUtilAppPrepareOverwritePkg(installPath.c_str());
        if (prep != 0) {
            sceAppInstUtilTerminate();
            result.code = prep;
            result.stage = "prepare overwrite";
            return result;
        }
    } else {
        // GMCA is already running, so normally the title has a primary slot.
        // Keep slot zero as a conservative fallback and let BGFT decide.
        slot = 0;
    }

    int32_t bgftError = 0;
    if (!initBgft(bgftError)) {
        sceAppInstUtilTerminate();
        result.code = bgftError;
        result.stage = "BGFT init";
        return result;
    }

    OrbisBgftDownloadParamEx params{};
    params.params.entitlementType = 5;
    params.params.id = "";
    params.params.contentUrl = installPath.c_str();
    params.params.contentName = "GMCA update";
    params.params.iconPath = "";
    params.params.playgoScenarioId = "0";
    params.params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
    params.slot = static_cast<uint32_t>(slot);

    OrbisBgftTaskId taskId = -1;
    rc = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &taskId);
    if (rc == 0) rc = sceBgftServiceDownloadStartTask(taskId);

    sceAppInstUtilTerminate();
    if (rc != 0) {
        result.code = rc;
        result.stage = "BGFT queue";
        return result;
    }

    result.queued = true;
    return result;
}

brls::Dialog* makeUpdateDialog(const std::string& title, const std::string& body) {
    auto* box = dynamic_cast<brls::Box*>(brls::View::createFromXMLResource("view/update_dialog.xml"));
    if (!box) return new brls::Dialog(title + "\n\n" + body);

    if (auto* t = dynamic_cast<brls::Label*>(box->getView("update/title"))) t->setText(title);
    std::string notes = misc::markdownToText(body);
    size_t first = notes.find_first_not_of("\n\r \t");
    size_t last = notes.find_last_not_of("\n\r \t");
    notes = first == std::string::npos ? "" : notes.substr(first, last - first + 1);
    if (notes.empty())
        box->getView("update/scroll")->setVisibility(brls::Visibility::GONE);
    else if (auto* b = dynamic_cast<brls::Label*>(box->getView("update/body")))
        b->setText(notes);
    return new brls::Dialog(box);
}

void startUpdate(const Manifest& manifest) {
    AppVersion::updating->store(false);

    brls::Style style = brls::Application::getStyle();
    auto* label = new brls::Label();
    label->setFontSize(style["brls/dialog/fontSize"]);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setSingleLine(false);
    label->setText(brls::getStr("main/setting/others/downloading", manifest.version, 0));

    auto* box = new brls::Box();
    box->addView(label);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setPadding(style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"],
        style["brls/dialog/paddingTopBottom"], style["brls/dialog/paddingLeftRight"]);

    auto* dialog = new brls::Dialog(box);
    dialog->setCancelable(false);
    auto dismissed = std::make_shared<std::atomic_bool>(false);
    dialog->addButton("hints/cancel"_i18n, [dismissed]() {
        dismissed->store(true);
        AppVersion::updating->store(true);
    });
    dialog->open();

    ThreadPool::instance().submit([manifest, label, dialog, dismissed](HTTP&) {
        ensurePackageDir();
        const std::string path = "/data/pkg/" + manifest.pkgName;

        auto finish = [dialog, dismissed](std::function<void()> then) {
            brls::sync([dialog, dismissed, then]() {
                if (dismissed->exchange(true))
                    then();
                else
                    dialog->close(then);
            });
        };

        auto last = std::make_shared<std::chrono::steady_clock::time_point>();
        HTTP::Progress::Callback progress = [manifest, label, dismissed, last](curl_off_t total, curl_off_t now) {
            auto tp = std::chrono::steady_clock::now();
            if (total <= 0 || tp - *last < std::chrono::milliseconds(500)) return;
            *last = tp;
            int percent = static_cast<int>(now * 100 / total);
            brls::sync([manifest, label, dismissed, percent]() {
                if (!dismissed->load())
                    label->setText(brls::getStr("main/setting/others/downloading", manifest.version, percent));
            });
        };

        try {
            std::remove(path.c_str());
            HTTP::download(kPackageUrl, path, HTTP::Timeout{-1, 10000}, AppVersion::updating, progress);

            if (AppVersion::updating->load()) {
                std::remove(path.c_str());
                return;
            }

            int64_t actualSize = fileSize(path);
            if (actualSize != manifest.size)
                throw std::runtime_error(fmt::format("PKG size mismatch ({}/{})", actualSize, manifest.size));

            std::string actualHash = sha256File(path);
            if (actualHash != manifest.sha256)
                throw std::runtime_error("PKG SHA-256 mismatch");

            brls::sync([label, dismissed]() {
                if (!dismissed->load()) label->setText("main/setting/others/installing"_i18n);
            });

            InstallResult install = queueInstall(path);
            AppVersion::updating->store(true);
            if (install.stage == "unexpected Title ID") {
                std::remove(path.c_str());
                throw std::runtime_error("downloaded PKG has an unexpected Title ID");
            }

            if (install.queued) {
                finish([manifest]() {
                    Dialog::quitApp(false,
                        fmt::format("GMCA PS4 {} verified. Installation is queued; close GMCA to let the PS4 finish the update.",
                            manifest.version));
                });
            } else {
                const std::string code = fmt::format("0x{:08X}", static_cast<uint32_t>(install.code));
                finish([manifest, path, code, stage = install.stage]() {
                    Dialog::show(fmt::format(
                        "GMCA PS4 {} was downloaded and SHA-256 verified. Automatic installation could not start "
                        "({}: {}). The PKG is already in {}. Open GoldHEN Package Installer with HDD /data/pkg as "
                        "the source to install it; no FTP transfer is needed.",
                        manifest.version, stage, code, path));
                });
            }
        } catch (const std::exception& ex) {
            bool canceled = AppVersion::updating->load();
            AppVersion::updating->store(true);
            if (canceled) return;
            std::remove(path.c_str());
            std::string msg = ex.what();
            finish([msg]() { Dialog::show(msg); });
        }
    });
}

}  // namespace

std::string currentVersion() {
    return GMCA_STR(GMCA_PS4_VERSION);
}

void checkUpdate(int delay, bool showUpToDateDialog) {
    if (!AppVersion::updating->load()) {
        Dialog::cancelable("main/setting/others/updating"_i18n, [] { AppVersion::updating->store(true); });
        return;
    }

    ThreadPool::instance().submit([delay, showUpToDateDialog](HTTP&) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        try {
            Manifest manifest = fetchManifest();
            if (versionNumber(manifest.version) <= versionNumber(currentVersion())) {
                brls::Logger::info("PS4 package is up to date ({})", currentVersion());
                if (showUpToDateDialog)
                    brls::sync([]() { Dialog::show("main/setting/others/up2date"_i18n); });
                return;
            }

            auto& conf = AppConfig::instance();
            std::string dismissed = conf.getItem(AppConfig::APP_UPDATE, std::string());
            if (!showUpToDateDialog && dismissed == manifest.version) {
                brls::Logger::info("PS4 update {} was dismissed for this install", manifest.version);
                return;
            }

            brls::sync([manifest]() {
                std::string title = brls::getStr("main/setting/others/upgrade", manifest.version);
                std::string notes = manifest.notes;
                if (notes.empty()) {
                    notes = "PS4 Stremio-only";
                    if (!manifest.commit.empty()) notes += "\nCommit: " + manifest.commit;
                }
                auto* dialog = makeUpdateDialog(title, notes);
                dialog->addButton("hints/cancel"_i18n, [version = manifest.version]() {
                    AppConfig::instance().setItem(AppConfig::APP_UPDATE, version);
                });
                dialog->addButton("hints/ok"_i18n, [manifest]() { startUpdate(manifest); });
                dialog->open();
            });
        } catch (const std::exception& ex) {
            brls::Logger::error("PS4 update check failed: {}", ex.what());
            if (showUpToDateDialog) {
                std::string msg = ex.what();
                brls::sync([msg]() { Dialog::show(msg); });
            }
        }
    });
}

}  // namespace ps4update

#endif
