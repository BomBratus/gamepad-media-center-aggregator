#ifdef __PS4__

#include <SDL2/SDL.h>
#include <orbis/_types/sys_service.h>

#include <cstdint>
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

struct BgftTaskProgress {
    uint32_t bits;
    int32_t errorResult;
    uint32_t length;
    uint32_t transferred;
    uint32_t lengthTotal;
    uint32_t transferredTotal;
    uint32_t numIndex;
    uint32_t numTotal;
    uint32_t restSec;
    uint32_t restSecTotal;
    int32_t preparingPercent;
    int32_t localCopyPercent;
};

int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv,
    unsigned int flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* name, void** func);
char* sceKernelGetFsSandboxRandomWord(void);
int32_t sceSystemServiceLaunchApp(const char* titleId, const char** argv, LncAppParam* param);

}

namespace {

constexpr const char* kPkgLocal = "/data/pkg/GMCA-PS4-Stremio-only.pkg";
constexpr const char* kPkgSystem = "/user/data/pkg/GMCA-PS4-Stremio-only.pkg";
constexpr const char* kLogPath = "/data/GMCA/ps4-updater.log";
constexpr const char* kCompleteMarker = "/data/GMCA/update-complete";
constexpr const char* kTargetTitle = "GMCA00000";
constexpr size_t kBgftHeapSize = 1024 * 1024;
constexpr int kOptInvisible = 0x2;
constexpr int kOptForceUpdate = 0x8;

FILE* gLog = nullptr;

int (*pAppInit)() = nullptr;
int (*pGetTitleId)(const char*, char*, int*) = nullptr;
int (*pGetPrimarySlot)(const char*, int*) = nullptr;
int (*pPrepareOverwrite)(const char*) = nullptr;
int (*pAppIsUpdating)(const char*, int*) = nullptr;
int (*pAppExists)(const char*, int*) = nullptr;
int (*pBgftInit)(BgftInitParams*) = nullptr;
int (*pBgftRegister)(BgftDownloadParamEx*, int*) = nullptr;
int (*pBgftStart)(int) = nullptr;
int (*pBgftGetProgress)(int, BgftTaskProgress*) = nullptr;

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

bool writeCompletionMarker() {
    FILE* f = std::fopen(kCompleteMarker, "w");
    if (!f) {
        logLine("could not write completion marker %s", kCompleteMarker);
        return false;
    }
    std::fputs("GMCA00000 update complete\n", f);
    std::fflush(f);
    std::fclose(f);
    return true;
}

int32_t launchMainTitle() {
    const char* argv[] = {nullptr};
    LncAppParam param{};
    param.size = sizeof(param);
    param.user_id = static_cast<uint32_t>(-1);
    param.LaunchAppCheck_flag = LaunchApp_None;
    return sceSystemServiceLaunchApp(kTargetTitle, argv, &param);
}

}  // namespace

int main(int, char*[]) {
    gLog = std::fopen(kLogPath, "w");
    logLine("GMCA PS4 updater helper start");

    int sdl = SDL_Init(SDL_INIT_VIDEO);
    logLine("SDL_Init -> %d (%s)", sdl, sdl == 0 ? "ok" : SDL_GetError());

    // GMCA now exits through Borealis' normal shutdown path. Give the title and
    // its app mount time to disappear before preparing the overwrite.
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
        resolve(hApp, "sceAppInstUtilAppIsInUpdating", reinterpret_cast<void**>(&pAppIsUpdating)) &
        resolve(hApp, "sceAppInstUtilAppExists", reinterpret_cast<void**>(&pAppExists)) &
        resolve(hBgft, "sceBgftServiceInit", reinterpret_cast<void**>(&pBgftInit)) &
        resolve(hBgft, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", reinterpret_cast<void**>(&pBgftRegister)) &
        resolve(hBgft, "sceBgftServiceDownloadStartTask", reinterpret_cast<void**>(&pBgftStart));
    bool progressOk =
        resolve(hBgft, "sceBgftServiceDownloadGetProgress", reinterpret_cast<void**>(&pBgftGetProgress)) ||
        resolve(hBgft, "sceBgftServiceIntDownloadGetProgress", reinterpret_cast<void**>(&pBgftGetProgress));
    ok = ok && progressOk;
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
        if (prep != 0) return 6;
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

    // Starting a BGFT task only queues the overwrite. Keep this title alive until
    // the task has actually copied the package AND AppInstUtil says GMCA is no
    // longer updating. This prevents the old helper's two-second premature exit.
    bool sawActivity = false;
    bool sawUpdating = false;
    bool completed = false;
    int stableDone = 0;
    for (int i = 0; i < 180; ++i) {
        sleep(1);

        BgftTaskProgress progress {};
        int progressRc = pBgftGetProgress(taskId, &progress);
        if (progressRc == 0) {
            logLine("progress bits=0x%08X err=0x%08X total=%u/%u prep=%d copy=%d",
                progress.bits, static_cast<unsigned>(progress.errorResult),
                progress.transferredTotal, progress.lengthTotal,
                progress.preparingPercent, progress.localCopyPercent);
            if (progress.errorResult != 0) return 10;
            if (progress.transferredTotal > 0 || progress.transferred > 0 ||
                progress.preparingPercent > 0 || progress.localCopyPercent > 0)
                sawActivity = true;
        }

        int updating = 0;
        int updateRc = pAppIsUpdating(kTargetTitle, &updating);
        if (updateRc == 0 && updating) {
            sawUpdating = true;
            sawActivity = true;
        }

        bool bytesDone = progressRc == 0 && progress.lengthTotal > 0 &&
                         progress.transferredTotal >= progress.lengthTotal;
        bool copyDone = progressRc == 0 && progress.localCopyPercent >= 100;
        bool doneSignal = bytesDone || copyDone || sawUpdating;

        if (sawActivity && updateRc == 0 && !updating && doneSignal)
            ++stableDone;
        else
            stableDone = 0;

        if (stableDone >= 2) {
            completed = true;
            break;
        }
    }

    if (!completed) {
        logLine("install completion timeout");
        return 11;
    }

    int exists = 0;
    rc = pAppExists(kTargetTitle, &exists);
    logLine("target exists after update -> rc=0x%08X exists=%d", static_cast<unsigned>(rc), exists);
    if (rc != 0 || !exists) return 12;

    if (!writeCompletionMarker())
        logLine("continuing without completion marker; recovery PKG will be preserved");

    int32_t launchRc = launchMainTitle();
    logLine("launch updated GMCA -> 0x%08X", static_cast<unsigned>(launchRc));
    if (launchRc < 0) return 13;

    SDL_Quit();
    return 0;
}

#endif  // __PS4__
