#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <borealis.hpp>
#include "api/http.hpp"
#include "api/backend.hpp"
#include "config.hpp"
#include "image_cache.hpp"

#if defined(__PS4__)
struct ImageRequestGroup;
#endif

class Image {
    using Ref = std::shared_ptr<Image>;

public:
    Image();
    Image(const Image&) = delete;

    virtual ~Image();

    /// Loads an image for a backend path: the on-disk cached asset if present
    /// (offline), else the active backend's image URL. width/height > 0 requests
    /// backend-side resize where the backend supports it.
    static void load(brls::Image* view, const std::string& path, int width = 0, int height = 0) {
        if (path.empty()) return;
        // offline cache wins: a locally cached asset renders without the server
        // and gives downloaded content instant local artwork even online
        // (SPEC §4.2, AC6/AC17). Keyed by the raw path/url passed here.
        if (ImageCache::has(path)) {
            std::string local = ImageCache::localPath(path);
#if defined(__PS4__)
            // PS4: run cached assets through the same decode/downscale
            // and upload path as network images. setImageFromFile would upload
            // the native-resolution file directly, bypassing those limits.
            withLocal(view, local, width, height);
#else
            view->setImageFromFile(local);
#endif
            return;
        }
        std::string url = AppConfig::instance().backend().imageUrl(path, width, height);
        // width/height are also forwarded to the decoder: backends that can't
        // resize server-side (Stremio's absolute Cinemeta/RPDB urls) still get
        // the artwork downscaled to its display size before the GPU upload, so a
        // 580x859 RPDB poster becomes a 512² texture instead of a 1024² one — the
        if (!url.empty()) with(view, url, width, height);
    }

    /// @brief 设置要加载内容的图片组件。此函数需要工作在主线程。
    /// width/height (>0) = intended display size, used on PS4 to cap
    /// the decoded texture before upload.
    static void with(brls::Image* view, const std::string& url, int width = 0, int height = 0);

#if defined(__PS4__)
    /// Cached-file path: like with(), but reads pixels from disk instead of the
    /// network and runs them through doRequest's platform-specific size limits
    /// and upload path. Main thread.
    static void withLocal(brls::Image* view, const std::string& localPath, int width = 0, int height = 0);
#endif

    /// @brief 取消请求，并清空图片。此函数需要工作在主线程。
    static void cancel(brls::Image* view);

private:
    void doRequest(HTTP& s);

    static void clear(brls::Image* view);

private:
    std::string url;
    // written by clear() (UI thread) while doRequest (worker) reads it on its
    // cancel/error paths — atomic so neither side sees a torn pointer
    std::atomic<brls::Image*> image;
    HTTP::Cancel isCancel;
    int targetW = 0;  // intended display size (platform texture cap); 0 = unknown
    int targetH = 0;
    // true: `url` is a local cache file read from disk instead of fetched over
    // HTTP; the decode/downscale/upload path is otherwise shared.
    bool local = false;

#if defined(__PS4__)
    // PS4-only in-flight coalescing: several Stremio rows often reference the
    // same absolute poster URL before the first texture reaches TextureCache.
    // Followers share one network/decode/upload job instead of consuming more
    // slots from the console's four-worker pool.
    std::shared_ptr<ImageRequestGroup> group;
    std::string groupKey;
#endif

    inline static std::mutex requestMutex;
    inline static std::unordered_map<brls::Image*, Ref> requests;
#if defined(__PS4__)
    inline static std::unordered_map<std::string, std::weak_ptr<ImageRequestGroup>> requestGroups;
#endif
};
