/* GMCA media models and playback. Persisted field names remain compatible. */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "api/http.hpp"
#include "api/media/types.hpp"

namespace media {

using OnError = std::function<void(const std::string&)>;
template <typename T>
using Then = std::function<void(T)>;

/// Backend kind (also the discriminant persisted in AppServer::type)
enum class BackendType { Stremio };

enum class MediaKind { Any, Movie, Show, Season, Episode, Collection, Playlist, Photo, Artist, Album, Track };

/// Playback report state
enum class PlayState { Playing, Paused, Stopped };

enum class ListKind { None, Favorites };

struct GridQuery {
    std::string sortField;
    bool descending = false;
    bool unwatchedOnly = false;
    std::string genreId;  // genre directory id to filter on (empty = none)
    MediaKind kind = MediaKind::Any;
};

/// Options for resolving a playback URL
struct PlaybackOptions {
    int64_t seekMs = 0;            // resume position
    int64_t audioStreamId = 0;     // selected audio stream id (0 = default)
    int64_t subtitleStreamId = 0;  // selected subtitle stream id (0 = none)
    int64_t bitrateCap = 0;        // bps cap; <= 0 = direct play / auto
    bool forceDirectPlay = false;
    bool burnSubtitles = false;
    std::string videoCodec = "h264";  // transcode target codec
    std::string sessionId;            // compatible media field
};

/// Result of resolvePlayback(): an mpv-playable URL + how to play it.
struct PlaybackSource {
    std::string url;          // URL handed to mpv
    std::string mpvExtra;     // extra mpv options string (network-timeout, start=, http-proxy…)
    bool isTranscode = false;
    std::string playMethod;   // "directplay" | "transcode"
    std::string transcodeSession;  // server-side transcode session token, to tear it down on reload/exit;
                                   // empty for direct play or backends without a server transcode session
};

/// Per-backend capability descriptor — pilots the UI (tabs, menus, controls).
struct Capabilities {
    // navigation
    bool sections = true;          // browsable libraries / catalogs as tabs
    bool homeHubs = true;          // server-provided home rows
    bool continueWatching = true;
    bool serverSort = true;        // sort/order honored server-side (else hide the sort control)
    bool serverFilter = true;      // unwatched/genre filters
    bool genres = true;
    bool collections = true;
    bool playlists = true;         // gates the Playlists tab
    bool related = true;
    bool personPages = true;
    bool globalSearch = true;      // unified search across types
    bool recentlyAdded = true;
    // item actions
    bool markWatched = true;
    ListKind listKind = ListKind::None;
    bool ratings = true;           // critic/audience ratings
    bool skipIntro = false;        // intro/credits markers
    // playback
    bool transcode = true;
    bool serverProgress = true;    // report progress to server (else local-only)
    bool downloadOriginal = true;
    // accounts
    bool multiProfile = true;      // compatible media field
};

/// Abstract media backend. Navigation verbs are pure virtual (every backend must
/// implement them). Capability-gated verbs have a default that reports
/// "unsupported"; a backend overrides them only when it declares the capability.
class Backend {
public:
    virtual ~Backend() = default;

    virtual BackendType type() const = 0;
    virtual const Capabilities& caps() const = 0;

    // ---- navigation ----------------------------------------------------------
    virtual void listSections(Then<Container<Section>> then, OnError error) = 0;
    /// Optional backend-provided sub-tabs for a library section, as
    /// (sectionKey, label) pairs (Stremio: one per catalog of the type). Empty
    /// (the default) -> the UI uses its built-in Suggestions/Collections/Genres
    /// sub-tabs. Synchronous: called on the UI thread from reads-only cached state.
    virtual std::vector<std::pair<std::string, std::string>> sectionTabs(const std::string& sectionId) {
        return {};
    }
    virtual void getHomeHubs(int count, bool excludeContinueWatching, Then<Container<Hub>> then, OnError error) = 0;
    virtual void getSectionHubs(const std::string& sectionId, int count, Then<Container<Hub>> then, OnError error) = 0;
    /// Provider-backed top-rated chart. Backends that do not expose one keep
    /// this default unsupported implementation. Stremio maps it to IMDb's
    /// all-time Top 250 movie / TV charts and pages the cached result locally.
    virtual void getTopRated(
        MediaKind kind, size_t start, size_t size, Then<Container<Item>> then, OnError error) {
        if (error) error("top rated unsupported");
    }
    virtual void getContinueWatching(int count, Then<Container<Hub>> then, OnError error) = 0;
    virtual void getLibraryGrid(
        const std::string& sectionId, const GridQuery& q, size_t start, size_t size, Then<Container<Item>> then,
        OnError error) = 0;
    virtual void getCollectionChildren(
        const std::string& collectionId, size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;
    virtual void getHubPage(
        const std::string& hubKey, size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;
    /// Single item detail. `full` requests heavy includes (streams/chapters/markers).
    virtual void getItemDetail(const std::string& id, bool full, Then<Item> then, OnError error) = 0;
    virtual void getChildren(const std::string& id, Then<Container<Item>> then, OnError error) = 0;
    virtual void getArtistAlbums(const std::string& artistId, Then<Container<Item>> then, OnError error) {
        getChildren(artistId, then, error);
    }
    /// All tracks of an artist (flattened across albums), for "play/shuffle
    /// artist". Backends override; default reports unsupported.
    virtual void getArtistTracks(const std::string& artistId, Then<Container<Item>> then, OnError error) {
        if (error) error("artist tracks unsupported");
    }
    virtual void getAllEpisodes(
        const std::string& showId, bool includeStreams, Then<Container<Item>> then, OnError error) = 0;
    /// Next episode to play. `then(item, fromStart)`: fromStart=true means the
    /// show is fully watched and `item` is its first episode ("Replay"). On no
    /// episode / failure, then(Item{}, false). Never surfaces a hard error to the UI.
    virtual void getNextUp(const std::string& showId, std::function<void(Item, bool)> then, OnError error) = 0;
    virtual void getExtras(const std::string& id, Then<Container<Item>> then, OnError error) = 0;
    virtual void getRelated(const std::string& id, int count, Then<Container<Hub>> then, OnError error) = 0;
    virtual void getPersonMedia(const std::string& personId, int count, Then<Container<Item>> then, OnError error) = 0;
    virtual void search(const std::string& query, MediaKind kind, int limit, Then<Container<Item>> then, OnError error) = 0;
    virtual void getRecentlyAdded(size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;
    virtual void getGenres(const std::string& sectionId, MediaKind kind, Then<Container<Section>> then, OnError error) = 0;
    virtual void getCollections(
        const std::string& sectionId, size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;
    virtual void getPlaylists(size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;
    virtual void getPlaylistItems(
        const std::string& playlistId, size_t start, size_t size, Then<Container<Item>> then, OnError error) = 0;

    // ---- item actions --------------------------------------------------------
    virtual void markWatched(const std::string& id) = 0;
    virtual void markUnwatched(const std::string& id) = 0;

    // ---- playback ------------------------------------------------------------
    /// Synchronous (call inside brls::async). `version` is the Media chosen by
    /// the player from item.media. Throws std::runtime_error on failure.
    virtual PlaybackSource resolvePlayback(const Item& item, const Media& version, const PlaybackOptions& opts) = 0;
    /// mpv sub-add URL for an external (sidecar) subtitle stream.
    virtual std::string subtitleSidecarUrl(const std::string& streamKey) const { return ""; }
    virtual void getSubtitles(
        const Item& item, const Media& version, Then<std::vector<Stream>> then, OnError error) {
        if (then) then({});
    }
    /// Optional guidance for the player's subtitle menu when NO subtitle track is
    /// available, telling the user how to get some. Empty (the default) = no hint.
    /// Stremio returns a "install a subtitles addon" message when its addon
    /// collection has none. Localized by the backend; synchronous (UI thread).
    virtual std::string subtitleMenuHint() const { return ""; }
    virtual void reportProgress(
        const std::string& id, PlayState state, int64_t posMs, int64_t durMs, const std::string& sessionId) {}

    // ---- url helpers ---------------------------------------------------------
    /// Server-relative image path -> tokenized (and optionally resized) URL.
    virtual std::string imageUrl(const std::string& path, int width = 0, int height = 0) const = 0;
    /// Proxy/resize an ABSOLUTE external image URL (provider posters, genre art).
    /// Backends without a proxy return the URL unchanged.
    virtual std::string imageUrlExternal(const std::string& absoluteUrl, int width, int height) const {
        return absoluteUrl;
    }
    /// Original-quality download URL for a media part.
    virtual std::string downloadUrl(const std::string& partKey) const = 0;
    virtual HTTP::Header authHeaders() const = 0;


    virtual bool canList(const Item& item) const { return false; }
    virtual void listWatchlist(
        const std::string& sortField, MediaKind kind, size_t start, size_t size, Then<Container<Item>> then,
        OnError error) {
        if (error) error("personal list unsupported");
    }
    /// Is the item currently on the personal list?
    virtual void getWatchlistState(const Item& item, Then<bool> then, OnError error) {
        if (then) then(false);
    }
    /// Add/remove the item from the personal list.
    virtual void setWatchlisted(const Item& item, bool add, std::function<void()> then, OnError error) {
        if (error) error("personal list unsupported");
    }
};

}  // namespace media
