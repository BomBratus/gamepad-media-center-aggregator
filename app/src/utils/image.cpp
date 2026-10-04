#include "utils/image.hpp"
#include "utils/thread.hpp"
#include "utils/background_governor.hpp"
#if defined(__PS4__)
#include "utils/artwork_cache.hpp"
static ArtworkCache& artworkCache() {
    static ArtworkCache cache(AppConfig::instance().configDir() + "/cache/artwork");
    return cache;
}
#endif
#include <fstream>
#include <vector>
#include <fmt/format.h>
#include <borealis/core/cache_helper.hpp>
#ifdef USE_WEBP
#include <webp/decode.h>
#endif
#include <stb_image.h>

#if defined(__PS4__)
struct ImageRequestGroup {
    HTTP::Cancel cancel = std::make_shared<std::atomic_bool>(false);
    std::vector<std::weak_ptr<Image>> members;
};
#endif

static HTTP::Timeout imageRequestTimeout() {
#if defined(__PS4__)
    // Artwork is not latency-critical like API navigation. Three seconds was
    // short enough to abort healthy CDN downloads on PS4 Wi-Fi. Keep a short
    // connect/DNS budget for dead hosts, but allow the body time to arrive.
    return HTTP::Timeout{8000L, 2500L};
#else
    return HTTP::Timeout{};
#endif
}


#if defined(__PS4__)
// PS4/Piglet uploads ordinary RGBA textures. Stremio artwork URLs frequently
// ignore GMCA's requested dimensions, so decode at source size and shrink in RAM
// before the GPU upload. Repeated 2x box filtering is intentionally simple and
// bounded: it avoids keeping multi-megapixel posters/backdrops in the texture
// cache while preserving enough pixels for the actual on-screen card.
static uint8_t* ps4_halve_rgba(const uint8_t* src, int w, int h, int* outW, int* outH) {
    int dw = (w + 1) / 2;
    int dh = (h + 1) / 2;
    auto* dst = static_cast<uint8_t*>(malloc((size_t)dw * dh * 4));
    if (!dst) return nullptr;

    for (int y = 0; y < dh; y++) {
        int sy0 = y * 2;
        int sy1 = sy0 + 1 < h ? sy0 + 1 : h - 1;
        const uint8_t* r0 = src + (size_t)sy0 * w * 4;
        const uint8_t* r1 = src + (size_t)sy1 * w * 4;
        uint8_t* d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * 2;
            int sx1 = sx0 + 1 < w ? sx0 + 1 : w - 1;
            for (int ch = 0; ch < 4; ch++) {
                unsigned sum = r0[sx0 * 4 + ch] + r0[sx1 * 4 + ch] +
                               r1[sx0 * 4 + ch] + r1[sx1 * 4 + ch];
                d[x * 4 + ch] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }

    *outW = dw;
    *outH = dh;
    return dst;
}
#endif

Image::Image() : image(nullptr) {
    this->isCancel = std::make_shared<std::atomic_bool>(false);
    brls::Logger::verbose("new Image {}", fmt::ptr(this));
}

Image::~Image() { brls::Logger::verbose("delete Image {}", fmt::ptr(this)); }

void Image::with(brls::Image* view, const std::string& url, int width, int height) {
    int tex = brls::TextureCache::instance().getCache(url);
    if (tex > 0) {
        // The cache owns this texture. brls::Image defaults freeTexture to
        // true, so a view whose FIRST load is a cache hit (common on Stremio:
        // identical absolute URLs across row cells and detail pages) would
        // nvgDeleteImage a cached texture from clear()/its destructor and
        // leave a dead id in the cache — drawn later, that's a rendering fault.
        view->setFreeTexture(false);
        view->innerSetImage(tex);
        return;
    }

    // One Image state per view. On PS4, identical URLs are grouped below so the
    // expensive HTTP/decode/upload work is shared without sharing cancellation
    // or view lifetime.
    Ref item = std::make_shared<Image>();
    item->image = view;
    item->url = url;
    item->targetW = width;
    item->targetH = height;

    bool shouldSubmit = true;
    {
        std::lock_guard<std::mutex> lock(requestMutex);

        auto it = requests.insert(std::make_pair(view, item));
        if (!it.second) {
            brls::Logger::warning("insert Image {} failed", fmt::ptr(view));
            return;
        }

        view->ptrLock();
        // 图片组件不处理纹理销毁，由缓存统一管理
        view->setFreeTexture(false);

#if defined(__PS4__)
        std::shared_ptr<ImageRequestGroup> group;
        auto git = requestGroups.find(url);
        if (git != requestGroups.end()) {
            group = git->second.lock();
            if (!group || group->cancel->load()) {
                requestGroups.erase(git);
                group.reset();
            }
        }
        if (!group) {
            group = std::make_shared<ImageRequestGroup>();
            requestGroups[url] = group;
        } else {
            shouldSubmit = false;
        }

        item->group = group;
        item->groupKey = url;
        group->members.emplace_back(item);
#endif
    }

    if (shouldSubmit && !ThreadPool::instance().trySubmit(TaskPriority::Normal,
            [item](HTTP& s) { item->doRequest(s); })) {
        // Enqueue runs on the UI thread: release the view lifetime immediately
        // on backpressure. Scroll recycling already cancels group transfers.
        Image::clear(view);
    }
}
#if defined(__PS4__)
void Image::withLocal(brls::Image* view, const std::string& localPath, int width, int height) {
    // Mirrors with(): the cache is keyed by the local path (as setImageFromFile
    // did), so repeat loads of a cached asset hit the TextureCache directly.
    int tex = brls::TextureCache::instance().getCache(localPath);
    if (tex > 0) {
        view->setFreeTexture(false);
        view->innerSetImage(tex);
        return;
    }

    Ref item = std::make_shared<Image>();
    bool shouldSubmit = true;
    {
        std::lock_guard<std::mutex> lock(requestMutex);

        auto it = requests.insert(std::make_pair(view, item));
        if (!it.second) {
            brls::Logger::warning("insert Image {} failed", fmt::ptr(view));
            return;
        }

        item->image = view;
        item->url = localPath;  // doubles as the disk path (this->local == true)
        item->local = true;
        item->targetW = width;
        item->targetH = height;
        view->ptrLock();
        view->setFreeTexture(false);

#if defined(__PS4__)
        // Local requests use their cache path as a stable key, just as network
        // requests use the URL. Repeated cached rows share decode/upload work.
        std::shared_ptr<ImageRequestGroup> group;
        auto git = requestGroups.find(localPath);
        if (git != requestGroups.end()) {
            group = git->second.lock();
            if (!group || group->cancel->load()) {
                requestGroups.erase(git);
                group.reset();
            }
        }
        if (!group) {
            group = std::make_shared<ImageRequestGroup>();
            requestGroups[localPath] = group;
        } else {
            shouldSubmit = false;
        }

        item->group = group;
        item->groupKey = localPath;
        group->members.emplace_back(item);
#endif
    }

    if (shouldSubmit && !ThreadPool::instance().trySubmit(TaskPriority::Normal,
            [item](HTTP& s) { item->doRequest(s); })) {
        // Enqueue runs on the UI thread: release the view lifetime immediately
        // on backpressure. Scroll recycling already cancels group transfers.
        Image::clear(view);
    }
}
#endif

void Image::cancel(brls::Image* view) {
    brls::TextureCache::instance().removeCache(view->getTexture());
    view->clear();

    clear(view);
}

void Image::doRequest(HTTP& s) {
    // PS4 duplicate requests share one transfer. The group cancel flag only
    // flips when every waiting view has gone away, so recycling the leader cell
    // cannot abort artwork still needed by another row.
    HTTP::Cancel requestCancel = this->isCancel;
#if defined(__PS4__)
    if (this->group) requestCancel = this->group->cancel;
#endif
    if (requestCancel->load()) {
#if defined(__PS4__)
        return;
#else
        // clear() must stay on the UI thread because ptrLockCounter is not atomic.
        auto* imagePtr = this->image.load();
        brls::sync([imagePtr] { Image::clear(imagePtr); });
        return;
#endif
    }
    try {
        std::string data;
        [[maybe_unused]] bool fromNetwork = false;
        if (this->local) {
            // offline: read the cached asset straight off disk — no server, no
            // curl handle touched (getinfo below would deref a NULL type on it).
            std::ifstream f(this->url, std::ios::binary);
            std::ostringstream body;
            body << f.rdbuf();
            data = body.str();
            if (data.empty()) throw std::runtime_error("empty or unreadable cache file");
        } else {
#if defined(__PS4__)
            data = artworkCache().read(this->url);
#endif
            if (data.empty()) {
                std::ostringstream body;
                HTTP::set_option(s, requestCancel, imageRequestTimeout());
                s._get(this->url, &body);
                data = body.str();
                fromNetwork = true;
            }
        }
        uint8_t* imageData = nullptr;
        int imageW = 0, imageH = 0;
        bool isWebp = false;
#ifdef USE_WEBP
        // Prefer the RIFF....WEBP magic: the only reliable signal for a cached
        // file (its name is a hash) and for any response with no Content-Type.
        // The "Webp" URL hint / Content-Type only exist on the network path.
        char* ct = nullptr;
        bool webpMagic = data.size() >= 12 && memcmp(data.data(), "RIFF", 4) == 0 &&
                         memcmp(data.data() + 8, "WEBP", 4) == 0;
        if (webpMagic || url.find("Webp") != std::string::npos ||
            (fromNetwork && s.getinfo(&ct) && ct != nullptr && strcmp(ct, "image/webp") == 0)) {
            imageData = WebPDecodeRGBA((const uint8_t*)data.c_str(), data.size(), &imageW, &imageH);
            isWebp = true;
        } else
#endif
        {
            int n;
            imageData = stbi_load_from_memory((unsigned char*)data.c_str(), data.size(), &imageW, &imageH, &n, 4);
        }

#if defined(__PS4__)
        // Optional cache eviction/write is skipped while playback is opening or
        // unhealthy. Visible artwork still gets its UI upload; no prefetch starts.
        if (imageData && fromNetwork && !requestCancel->load() && gmca::backgroundGovernor().backgroundAllowed())
            artworkCache().store(this->url, data);
        if (imageData && imageW > 0 && imageH > 0) {
            int tW = this->targetW;
            int tH = this->targetH;
            if (tW > 0 && tH == 0) tH = (int)((int64_t)tW * imageH / imageW);
            if (tH > 0 && tW == 0) tW = (int)((int64_t)tH * imageW / imageH);

            // Never shrink below 128 px on either known display axis (small UI
            // icons stay crisp), while unknown-size images are still bounded to
            // 2048 px — above the PS4 app's 1080p output needs.
            int capW = tW > 0 ? (tW < 128 ? 128 : tW) : 2048;
            int capH = tH > 0 ? (tH < 128 ? 128 : tH) : 2048;
            while (imageData && (imageW > capW || imageH > capH)) {
                int nw = 0;
                int nh = 0;
                uint8_t* half = ps4_halve_rgba(imageData, imageW, imageH, &nw, &nh);
                if (!half) break;
#ifdef USE_WEBP
                if (isWebp)
                    WebPFree(imageData);
                else
#endif
                    stbi_image_free(imageData);
                imageData = half;
                imageW = nw;
                imageH = nh;
                // ps4_halve_rgba allocates with malloc; stbi_image_free maps to free.
                isWebp = false;
            }
        }
#endif

        bool hasAlpha = isWebp;
        // exact GPU footprint of the upload, forwarded to the TextureCache
        // byte capacity; 0 = let addCache estimate (w*h*4)
        size_t texBytes = 0;
#if defined(__PS4__)
        if (imageData) texBytes = (size_t)imageW * imageH * 4;
#endif

        int imageFlags = 0;
        (void)hasAlpha;

#if defined(__PS4__)
        auto groupCopy = this->group;
        auto groupKeyCopy = this->groupKey;
        auto urlCopy = this->url;
        auto isWebpCopy = isWebp;
        brls::Logger::verbose("request Image {} size {}", urlCopy, data.size());
        brls::sync([groupCopy, groupKeyCopy, urlCopy, imageData, imageW, imageH, isWebpCopy, imageFlags, texBytes] {
            std::vector<Ref> members;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                if (groupCopy) {
                    members.reserve(groupCopy->members.size());
                    for (const auto& weak : groupCopy->members) {
                        if (auto member = weak.lock()) members.push_back(std::move(member));
                    }
                }
                auto it = requestGroups.find(groupKeyCopy);
                if (it != requestGroups.end() && it->second.lock() == groupCopy) requestGroups.erase(it);
            }

            bool textureReferenceHeld = false;
            bool createAttempted = false;
            for (const auto& member : members) {
                auto* imagePtr = member->image.load();
                if (!imagePtr || member->isCancel->load()) continue;

                int viewTex = 0;
                if (!textureReferenceHeld) {
                    // getCache increments the cache refcount on a hit; addCache
                    // owns the first reference on a miss.
                    viewTex = brls::TextureCache::instance().getCache(urlCopy);
                    if (viewTex == 0 && imageData != nullptr && !createAttempted) {
                        createAttempted = true;
                        NVGcontext* vg = brls::Application::getNVGContext();
                        viewTex = nvgCreateImageRGBA(vg, imageW, imageH, imageFlags, imageData);
                        brls::TextureCache::instance().addCache(urlCopy, viewTex, texBytes);
                    }
                    textureReferenceHeld = viewTex > 0;
                } else {
                    // Give each duplicate card its own TextureCache reference so
                    // recycling one card cannot invalidate another card's texture.
                    viewTex = brls::TextureCache::instance().getCache(urlCopy);
                }

                if (viewTex > 0) imagePtr->innerSetImage(viewTex);
                clear(imagePtr);
            }

            if (imageData) {
#ifdef USE_WEBP
                if (isWebpCopy)
                    WebPFree(imageData);
                else
#endif
                    stbi_image_free(imageData);
            }
        });
#else
        auto* imagePtr = this->image.load();
        auto urlCopy = this->url;
        auto isCancelCopy = this->isCancel;

        brls::Logger::verbose("request Image {} size {}", urlCopy, data.size());
        brls::sync([imagePtr, urlCopy, isCancelCopy, imageData, imageW, imageH, isWebp, imageFlags, texBytes] {
            if (!isCancelCopy->load()) {
                // Load texture
                int tex = brls::TextureCache::instance().getCache(urlCopy);
                if (tex == 0 && imageData != nullptr) {
                    NVGcontext* vg = brls::Application::getNVGContext();
                    tex = nvgCreateImageRGBA(vg, imageW, imageH, imageFlags, imageData);
                    brls::TextureCache::instance().addCache(urlCopy, tex, texBytes);
                }
                if (tex > 0) imagePtr->innerSetImage(tex);
                clear(imagePtr);
            }
            if (imageData) {
#ifdef USE_WEBP
                if (isWebp)
                    WebPFree(imageData);
                else
#endif
                    stbi_image_free(imageData);
            }
        });
#endif
    } catch (const std::exception& ex) {
        brls::Logger::warning("request image {} {}", this->url, ex.what());
#if defined(__PS4__)
        auto groupCopy = this->group;
        auto groupKeyCopy = this->groupKey;
        brls::sync([groupCopy, groupKeyCopy] {
            std::vector<Ref> members;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                if (groupCopy) {
                    members.reserve(groupCopy->members.size());
                    for (const auto& weak : groupCopy->members) {
                        if (auto member = weak.lock()) members.push_back(std::move(member));
                    }
                }
                auto it = requestGroups.find(groupKeyCopy);
                if (it != requestGroups.end() && it->second.lock() == groupCopy) requestGroups.erase(it);
            }
            for (const auto& member : members) {
                auto* imagePtr = member->image.load();
                if (imagePtr) Image::clear(imagePtr);
            }
        });
#else
        auto* imagePtr = this->image.load();
        brls::sync([imagePtr] { Image::clear(imagePtr); });
#endif
    }
}

void Image::clear(brls::Image* view) {
    if (!view) return;

    std::lock_guard<std::mutex> lock(requestMutex);

    auto it = requests.find(view);
    if (it == requests.end()) return;

    Ref item = it->second;
    view->ptrUnlock();
    item->image = nullptr;
    item->isCancel->store(true);
    requests.erase(it);

#if defined(__PS4__)
    if (item->group) {
        bool active = false;
        for (const auto& weak : item->group->members) {
            auto member = weak.lock();
            if (member && !member->isCancel->load() && member->image.load() != nullptr) {
                active = true;
                break;
            }
        }
        if (!active) {
            item->group->cancel->store(true);
            auto git = requestGroups.find(item->groupKey);
            if (git != requestGroups.end() && git->second.lock() == item->group) requestGroups.erase(git);
        }
    }
#endif
}
