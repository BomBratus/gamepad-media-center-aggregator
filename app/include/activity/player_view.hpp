/* GMCA media models and playback. Persisted field names remain compatible. */

#pragma once

#include <borealis.hpp>
#include <utils/event.hpp>
#include <utils/playback_checkpoint.hpp>
#include <api/media/types.hpp>

class VideoView;

class PlayerView : public brls::Box {
public:
    PlayerView(const media::Item& item, const int64_t seekMs = 0, int versionIndex = -1);
    ~PlayerView();
#ifdef GMCA_TEST_HARNESS
    const std::string& testItem() const { return itemId; }
#endif

    /// Loads the show's episode list (previous/next navigation)
    void setSeries(const std::string& showRatingKey);
    void setTitie(const std::string& title);

private:
    void setChapters(const std::vector<media::Chapter>& chaps, int64_t durationMs);
    /// Fetches fresh metadata then resolves the playback URL via the backend
    void playMedia(const int64_t seekMs);
    void startPlayback(const int64_t seekMs, bool forceDirect = false);
    bool trySourceRecovery(int64_t resumeMs = -1);
    bool playIndex(int index);
    void updateUpNext(int64_t progressSeconds);
    int nextEpisodeIndex() const;
    void dismissUpNext();
    void resolveExternalSubtitles();
    /// sub-adds the resolved external subtitles into mpv, selecting the track
    /// matching the preferred-language setting (PLAYER_SUBTITLE_LANG). Called on
    /// every (re)load — mpv drops sub-add'ed tracks on each loadfile.
    void addExternalSubtitles();
    /// Report Stremio playback state (time/duration in ms)
    void reportTimeline(const std::string& state, int64_t timeMs);
    void reportStop(int64_t timeMs = -1);
    bool toggleQuality();

    // Playback
    std::string itemId;  // ratingKey
    /// playMethod: "directplay" | "transcode" (VideoProfile display)
    std::string playMethod;
    /// stable play-session id for the whole playback session
    std::string sessionId;
    media::Item item;     // fresh metadata (media/chapters/markers)
    media::Media stream;  // selected version
    /// caller-chosen source index (Stremio picker); -1 = first accessible.
    /// Reset to -1 on episode switch so binge auto-picks the best source.
    int preferredVersion = -1;
    bool scrobbled = false;
    utils::PlaybackCheckpoint playbackCheckpoint;
    void checkpointPlayback(int64_t timeMs);
    bool resolvingRecoverySources = false;
    uint64_t playbackGeneration = 0; // reject results from superseded loads
    std::vector<media::Item> episodes;
    int episodeIndex = -1;
    bool upNextDismissed = false;
    brls::Dialog* upNextDialog = nullptr;
    brls::Label* upNextLabel = nullptr;

    /// External subtitle sidecars (Stremio addons) for the current item, resolved
    /// lazily at play time and sub-add'ed on each (re)load. `externalSubsItem` is
    /// the ratingKey they belong to, so quality/track switches (same item) don't
    /// re-fetch while an episode switch does. `mpvLoaded` guards the async->sub-add
    /// timing (add on load OR when the fetch lands, whichever is last).
    std::vector<media::Stream> externalSubs;
    std::string externalSubsItem;
    bool mpvLoaded = false;

    MPVEvent::Subscription eventSubscribeID;
    brls::VoidEvent::Subscription exitSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    MPVCustomEvent::Subscription customEventSubscribeID;
    VideoView* view = nullptr;
};
