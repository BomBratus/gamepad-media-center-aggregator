#ifdef __PS4__

#include <SDL2/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {

struct BgftInitParams {
    void* heap;
    size_t heapSize;
};

struct BgftDownloadParam {
    int userId;
    int entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    int option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};

struct BgftDownloadParamEx {
    BgftDownloadParam param;
    unsigned int slot;
};

int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv,
    unsigned int flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* name, void** func);
char* sceKernelGetFsSandboxRandomWord(void);

}

namespace {

constexpr const char* kPkgLocal = "/data/pkg/GMCA-PS4-Stremio-only.pkg";
constexpr const char* kPkgSystem = "/user/data/pkg/GMCA-PS4-Stremio-only.pkg";
constexpr const char* kLogPath = "/data/GMCA/ps4-updater.log";
constexpr const char* kTargetTitle = "GMCA00000";
constexpr size_t kBgftHeapSize = 1024 * 1024;
constexpr int kOptInvisible = 0x2;
constexpr int kOptForceUpdate = 0x8;

FILE* gLog = nullptr;

int (*pAppInit)() = nullptr;
int (*pGetTitleId)(const char*, char*, int*) = nullptr;
int (*pGetPrimarySlot)(const char*, int*) = nullptr;
int (*pPrepareOverwrite)(const char*) = nullptr;
int (*pBgftInit)(BgftInitParams*) = nullptr;
int (*pBgftRegister)(BgftDownloadParamEx*, int*) = nullptr;
int (*pBgftStart)(int) = nullptr;

void logLine(const char* fmt, ...) {
    if (!gLog) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(gLog, fmt, ap);
    va_end(ap);
    std::fputc('\n', gLog);
    std::fflush(gLog);
}

int loadModule(const char* name) {
    const char* word = sceKernelGetFsSandboxRandomWord();
    char path[256];
    std::snprintf(path, sizeof(path), "/%s/common/lib/%s", word ? word : "system", name);
    int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    logLine("load %s -> %d", path, handle);
    return handle;
}

bool resolve(int handle, const char* name, void** out) {
    int rc = sceKernelDlsym(handle, name, out);
    logLine("dlsym %s -> 0x%08X %p", name, static_cast<unsigned>(rc), *out);
    return rc == 0 && *out != nullptr;
}

bool statPkg() {
    struct stat st {};
    if (stat(kPkgLocal, &st) != 0 || st.st_size <= 0) {
        logLine("pkg missing: %s", kPkgLocal);
        return false;
    }
    logLine("pkg size=%lld", static_cast<long long>(st.st_size));
    return true;
}

}  // namespace

int main(int, char*[]) {
    gLog = std::fopen(kLogPath, "w");
    logLine("GMCA PS4 updater helper start");

    // On this port SDL initialization establishes the process environment used
    // by the system-service PRXs. Do it before touching AppInstUtil/BGFT.
    int sdl = SDL_Init(SDL_INIT_VIDEO);
    logLine("SDL_Init -> %d (%s)", sdl, sdl == 0 ? "ok" : SDL_GetError());

    // GMCA exits immediately after launching us. Give the title and its app
    // mount a short grace period to disappear before preparing the overwrite.
    sleep(4);

    if (!statPkg()) return 2;

    int hApp = loadModule("libSceAppInstUtil.sprx");
    int hBgft = loadModule("libSceBgft.sprx");
    if (hApp <= 0 || hBgft <= 0) {
        logLine("installer module load failed");
        return 3;
    }

    bool ok =
        resolve(hApp, "sceAppInstUtilInitialize", reinterpret_cast<void**>(&pAppInit)) &
        resolve(hApp, "sceAppInstUtilGetTitleIdFromPkg", reinterpret_cast<void**>(&pGetTitleId)) &
        resolve(hApp, "sceAppInstUtilGetPrimaryAppSlot", reinterpret_cast<void**>(&pGetPrimarySlot)) &
        resolve(hApp, "sceAppInstUtilAppPrepareOverwritePkg", reinterpret_cast<void**>(&pPrepareOverwrite)) &
        resolve(hBgft, "sceBgftServiceInit", reinterpret_cast<void**>(&pBgftInit)) &
        resolve(hBgft, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", reinterpret_cast<void**>(&pBgftRegister)) &
        resolve(hBgft, "sceBgftServiceDownloadStartTask", reinterpret_cast<void**>(&pBgftStart));
    if (!ok) {
        logLine("required installer symbol missing");
        return 4;
    }

    int rc = pAppInit();
    logLine("AppInstUtilInitialize -> 0x%08X", static_cast<unsigned>(rc));

    char title[16] = {};
    int isApp = 0;
    const char* preparePath = kPkgSystem;
    rc = pGetTitleId(kPkgSystem, title, &isApp);
    if (rc != 0) {
        preparePath = kPkgLocal;
        std::memset(title, 0, sizeof(title));
        rc = pGetTitleId(kPkgLocal, title, &isApp);
    }
    logLine("pkg title -> rc=0x%08X title=%s isApp=%d", static_cast<unsigned>(rc), title, isApp);
    if (rc != 0 || std::strcmp(title, kTargetTitle) != 0 || !isApp) return 5;

    int slot = 0;
    rc = pGetPrimarySlot(kTargetTitle, &slot);
    logLine("GetPrimaryAppSlot -> 0x%08X slot=%d", static_cast<unsigned>(rc), slot);
    if (rc == 0) {
        int prep = pPrepareOverwrite(preparePath);
        logLine("PrepareOverwritePkg(%s) -> 0x%08X", preparePath, static_cast<unsigned>(prep));
        if (prep != 0) {
            // Never uninstall GMCA here. A failed overwrite must leave the old
            // title intact and the verified PKG available for manual recovery.
            return 6;
        }
    } else {
        slot = 0;
    }

    void* heap = std::calloc(1, kBgftHeapSize);
    if (!heap) {
        logLine("BGFT heap allocation failed");
        return 7;
    }
    BgftInitParams init {};
    init.heap = heap;
    init.heapSize = kBgftHeapSize;
    rc = pBgftInit(&init);
    logLine("BgftServiceInit -> 0x%08X", static_cast<unsigned>(rc));

    BgftDownloadParamEx params {};
    params.slot = static_cast<unsigned int>(slot);
    params.param.entitlementType = 5;
    params.param.id = "";
    params.param.contentUrl = kPkgSystem;
    params.param.contentName = "GMCA update";
    params.param.option = kOptInvisible | kOptForceUpdate;
    params.param.playgoScenarioId = "0";

    int taskId = -1;
    rc = pBgftRegister(&params, &taskId);
    logLine("BGFT register -> 0x%08X task=%d", static_cast<unsigned>(rc), taskId);
    if (rc != 0) return 8;

    rc = pBgftStart(taskId);
    logLine("BGFT start -> 0x%08X", static_cast<unsigned>(rc));
    if (rc != 0) return 9;

    logLine("install queued successfully; verified PKG remains at %s", kPkgLocal);
    sleep(2);
    return 0;
}

#endif  // __PS4__
