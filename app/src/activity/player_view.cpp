/* GMCA Stremio player. mpv positions are seconds; media progress is milliseconds. */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "activity/player_view.hpp"
#include "api/backend.hpp"
#include "api/stremio/backend.hpp"
#include "api/stremio/archive.hpp"
#include "api/stremio/episode_continuation.hpp"
#include "tab/media_series.hpp"
#include "utils/dialog.hpp"
#include "utils/config.hpp"
#include "utils/misc.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/audio_player.hpp"
#include "view/auto_tab_frame.hpp"

using namespace brls::literals;


#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
enum class Ps4SubtitleSidecarSafety { SafeText, Unknown, Risky };

static Ps4SubtitleSidecarSafety ps4SubtitleSidecarSafety(const std::string& rawUrl) {
    std::string url = rawUrl.substr(0, rawUrl.find_first_of("?#"));
    for (auto& ch : url) ch = (char)std::tolower((unsigned char)ch);
    auto endsWith = [&url](const char* suffix) {
        size_t n = std::strlen(suffix);
        return url.size() >= n && url.compare(url.size() - n, n, suffix) == 0;
    };
    if (endsWith(".srt") || endsWith(".vtt") || endsWith(".ass") || endsWith(".ssa"))
        return Ps4SubtitleSidecarSafety::SafeText;
    if (endsWith(".sup") || endsWith(".sub") || endsWith(".idx"))
        return Ps4SubtitleSidecarSafety::Risky;
    return Ps4SubtitleSidecarSafety::Unknown;
}
#endif

PlayerView::PlayerView(const media::Item& item, const int64_t seekMs, int versionIndex)
    : itemId(item.ratingKey), item(item), preferredVersion(versionIndex) {
    archivePlaybackSession = stremio::archive::Cache::instance().beginPlayback();
    // take sole ownership of MPVCore: if music was playing, the audio controller
    // must stop owning the shared event bus (else it reports this video's
    // progress against the audio track and auto-advances over it). SPEC.md §11.
    AudioPlayer::instance().release();
    float width = brls::Application::contentWidth;
    float height = brls::Application::contentHeight;
    view = new VideoView();
    view->setDimensions(width, height);
    view->setWidthPercentage(100);
    view->setHeightPercentage(100);
    view->setId("video");
    this->setDimensions(width, height);
    this->addView(view);
    view->registerVideoQuality([this](...) { return this->toggleQuality(); });
    view->registerVideoSubtitle([this](...) {
        PlayerSetting::showSubtitleMenu(&this->stream);
        return true;
    });
    view->registerVideoAudio([this](...) {
        PlayerSetting::showAudioMenu(&this->stream);
        return true;
    });
    view->registerError([this](...) {
        return this->trySourceRecovery();
    });

    // stable session identifier (24 characters)
    this->sessionId = misc::randHex(12);

    auto& mpv = MPVCore::instance();

    brls::Application::pushActivity(new brls::Activity(this), brls::TransitionAnimation::NONE);

    view->setNextEpisode([this]() { return this->nextEpisodeIndex(); });
    playSubscribeID = view->getPlayEvent()->subscribe([this](int index) { this->playIndex(index); });

    settingSubscribeID = view->getSettingEvent()->subscribe([]() {
        brls::View* setting = new PlayerSetting();
        brls::Application::pushActivity(new brls::Activity(setting));
    });

    eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
        auto& mpv = MPVCore::instance();
        // Loading/seek events park the writer immediately. Only a loaded,
        // non-buffering file may start the stable-playback grace period.
        if (event == MpvEventEnum::LOADING_START || event == MpvEventEnum::SEEK_START ||
            event == MpvEventEnum::START_FILE || event == MpvEventEnum::RESET ||
            event == MpvEventEnum::MPV_STOP || event == MpvEventEnum::END_OF_FILE ||
            event == MpvEventEnum::MPV_FILE_ERROR)
            stremio::archive::Cache::instance().playbackState(archivePlaybackSession, false);
        if (event == MpvEventEnum::UPDATE_PROGRESS || event == MpvEventEnum::MPV_PAUSE ||
            event == MpvEventEnum::MPV_RESUME || event == MpvEventEnum::LOADING_END ||
            event == MpvEventEnum::LOADING_START) {
            const bool healthy = mpvLoaded && playbackCheckpoint.ready() && !mpv.isStopped() &&
                !mpv.getInt("paused-for-cache", 0) && !mpv.getInt("seeking", 0) &&
                (!mpv.getInt("core-idle", 0) || mpv.isPaused());
            stremio::archive::Cache::instance().playbackState(archivePlaybackSession, healthy);
        }
        switch (event) {
        case MpvEventEnum::MPV_RESUME:
            this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
            view->getProfile()->init(this->playMethod);
            break;
        case MpvEventEnum::MPV_PAUSE:
            this->reportTimeline("paused", AppConfig::instance().backend().type() == media::BackendType::Stremio
                ? int64_t(mpv.getDouble("playback-time", -1) * 1000)
                : int64_t(mpv.video_progress) * 1000);
            break;
        case MpvEventEnum::LOADING_END:
            this->reportTimeline("playing", int64_t(mpv.playback_time) * 1000);
            break;
        case MpvEventEnum::RESET:
            if (AppConfig::instance().backend().type() != media::BackendType::Stremio) break;
            this->reportStop();
            this->playbackCheckpoint.stop();
            this->mpvLoaded = false;
            break;
        case MpvEventEnum::PLAYBACK_RESTART:
            this->playbackCheckpoint.restart();
            // A seek can deliver its position property before the restart event.
            // Sample the confirmed position now so closing before the next second
            // also preserves backward seeks, including a deliberate seek to zero.
            if (AppConfig::instance().backend().type() == media::BackendType::Stremio)
                this->checkpointPlayback(int64_t(mpv.getDouble("playback-time", -1) * 1000));
            break;
        case MpvEventEnum::SEEK_START:
        case MpvEventEnum::START_FILE:
            // LOADING_START also represents core-idle during a pause or
            // buffering. It must not disable checkpoints for a loaded file.
            this->playbackCheckpoint.suspend();
            break;
        case MpvEventEnum::MPV_STOP:
            this->playbackCheckpoint.suspend();
            this->reportStop();
            this->playbackCheckpoint.stop();
            this->mpvLoaded = false;
            break;
        case MpvEventEnum::END_OF_FILE:
            // VideoView defers autoplay to the next UI tick, so the current item
            // is still active here. Persist a final, unambiguous EOF checkpoint.
            this->mpvLoaded = false;
            this->reportStop(this->item.duration > 0 ? this->item.duration
                                                     : int64_t(mpv.playback_time) * 1000);
            this->playbackCheckpoint.stop();
            break;
        case MpvEventEnum::MPV_LOADED: {
            const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
            for (auto& part : this->stream.parts) {
                for (auto& s : part.streams) {
                    if (s.streamType != media::streamTypeSubtitle || s.key.empty()) continue;
                    std::string url = AppConfig::instance().backend().subtitleSidecarUrl(s.key);
#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
                    auto safety = ps4SubtitleSidecarSafety(url);
                    if (safety == Ps4SubtitleSidecarSafety::Risky)
                        brls::Logger::info("PS4 subtitle guard: bitmap sidecar is manual-select only {}", url);
                    // Only known text formats may be selected automatically.
                    // Bitmap and unknown sidecars stay available in the menu, but
                    // cannot alter the renderer during video startup.
                    const char* ps4Flag =
                        safety == Ps4SubtitleSidecarSafety::SafeText ? flag : "auto";
                    mpv.command("sub-add", url.c_str(), ps4Flag, s.displayTitle.c_str());
#else
                    mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str());
#endif
                }
            }
            // External subtitles resolved lazily by the backend (Stremio addons):
            // mpv dropped the previous load's tracks, so (re)add them here. If the
            // fetch is still in flight, its callback adds them once it lands.
            this->mpvLoaded = true;
            this->playbackCheckpoint.loaded();
            this->addExternalSubtitles();
            break;
        }
        case MpvEventEnum::UPDATE_PROGRESS:
            if (!this->playbackCheckpoint.ready() && AppConfig::instance().backend().type() == media::BackendType::Stremio) break;
            this->checkpointPlayback(int64_t(mpv.playback_time * 1000));
            // report cadence: every 10 s
            if (mpv.video_progress % 10 == 0) {
                this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
            }
            this->updateUpNext(mpv.video_progress);
            break;
        default:;
        }
    });
    customEventSubscribeID = mpv.getCustomEvent()->subscribe([this](const std::string& event, void* data) {
        if (event == QUALITY_CHANGE) {
            int64_t pos = int64_t(MPVCore::instance().playback_time) * 1000;
            MPVCore::instance().reset();
            this->playMedia(pos);
        } else if (event == "PreviousTrack") {
            this->view->playNext(-1);
        } else if (event == "NextTrack") {
            this->view->playNext(1);
        }
    });

    this->playMedia(seekMs > 0 ? seekMs : item.viewOffset);

    // Report stop when application exit
    this->exitSubscribeID = brls::Application::getExitEvent()->subscribe([this]() {
        if (!MPVCore::instance().isStopped()) this->reportStop();
    });
}

PlayerView::~PlayerView() {
    this->dismissUpNext();
    auto& mpv = MPVCore::instance();
    mpv.getEvent()->unsubscribe(eventSubscribeID);
    mpv.getCustomEvent()->unsubscribe(customEventSubscribeID);
    view->getPlayEvent()->unsubscribe(playSubscribeID);
    view->getSettingEvent()->unsubscribe(settingSubscribeID);

    brls::sync([&mpv]() { mpv.getCustomEvent()->fire(VIDEO_CLOSE, nullptr); });

    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    if (!mpv.isStopped()) this->reportStop();
    // Free the server-side transcode session on exit (else it lingers orphaned).
    brls::Application::getExitEvent()->unsubscribe(this->exitSubscribeID);
    brls::Logger::debug("trying delete PlayerView...");
    // Box destroys its VideoView (and stops mpv) after this destructor returns.
    const auto archiveSession = archivePlaybackSession;
    brls::sync([archiveSession] { stremio::archive::Cache::instance().endPlayback(archiveSession); });
}

void PlayerView::setSeries(const std::string& showRatingKey) {
    ASYNC_RETAIN
    // all episodes of the show
    AppConfig::instance().backend().getAllEpisodes(showRatingKey, false,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            int index = -1;
            std::vector<std::string> values;
            for (size_t i = 0; i < r.Items.size(); i++) {
                auto& it = r.Items.at(i);
                if (it.ratingKey == this->itemId) index = i;
                values.push_back(fmt::format("S{}E{} - {}", it.parentIndex, it.index, it.title));
            }
            view->setList(values, index);
            this->episodes = std::move(r.Items);
            this->episodeIndex = index;
            this->upNextDismissed = false;
            view->setAutoNext(AppConfig::instance().getItem(AppConfig::PLAYER_AUTOPLAY_NEXT, true));
        },
        [ASYNC_TOKEN](const std::string& error) {
            ASYNC_RELEASE
            Dialog::show(error);
        });
}

void PlayerView::setTitie(const std::string& title) { this->view->setTitie(title); }

void PlayerView::setChapters(const std::vector<media::Chapter>& chaps, int64_t durationMs) {
    std::vector<float> clips;
    if (durationMs > 0) {
        for (auto& c : chaps) {
            clips.push_back(float(c.startTimeOffset) / float(durationMs));
        }
    }
    this->view->setClipPoint(clips);
}

bool PlayerView::playIndex(int index) {
    if (index < 0 || index >= (int)this->episodes.size()) {
        return VideoView::close();
    }
    this->dismissUpNext();
    this->upNextDismissed = false;
    this->view->setAutoNext(AppConfig::instance().getItem(AppConfig::PLAYER_AUTOPLAY_NEXT, true));
    // reset synchronously reports the old episode; retain its navigation index
    // until that checkpoint has updated the loaded episode list.
    MPVCore::instance().reset();
    this->episodeIndex = index;

    auto next = this->episodes.at(index);
    this->itemId = next.ratingKey;
    this->item = next;
    this->scrobbled = false;
    this->sessionId = misc::randHex(12);
    this->preferredVersion = -1;  // binge: auto-pick the best source for the new episode
    this->playMedia(0);
    view->setTitie(next.grandparentTitle.empty()
                       ? fmt::format("S{}E{} — {}", next.parentIndex, next.index, next.title)
                       : fmt::format("{} · S{}E{} — {}", next.grandparentTitle, next.parentIndex, next.index,
                             next.title));
    return true;
}

void PlayerView::dismissUpNext() {
    if (!this->upNextDialog) return;
    auto* dialog = this->upNextDialog;
    this->upNextDialog = nullptr;
    this->upNextLabel = nullptr;
    dialog->close([]() {});
}

int PlayerView::nextEpisodeIndex() const {
    if (AppConfig::instance().backend().type() == media::BackendType::Stremio)
        return stremio::episodeContinuationIndex(this->episodes, this->itemId);
    return this->episodeIndex >= 0 && this->episodeIndex + 1 < (int)this->episodes.size()
        ? this->episodeIndex + 1 : -1;
}

void PlayerView::updateUpNext(int64_t progressSeconds) {
    const bool enabled = AppConfig::instance().getItem(AppConfig::PLAYER_AUTOPLAY_NEXT, true);
    const int target = this->nextEpisodeIndex();
    this->view->setAutoNext(enabled && !this->upNextDismissed && target >= 0);
    if (!enabled || this->upNextDismissed || target < 0) {
        this->dismissUpNext();
        return;
    }

    int64_t durationSeconds = this->item.duration > 0 ? this->item.duration / 1000
                                                      : (int64_t)MPVCore::instance().duration;
    if (durationSeconds <= 0) return;
    int remaining = (int)std::max<int64_t>(0, durationSeconds - progressSeconds);
    if (remaining > 10 || remaining <= 0) return;

    const auto& next = this->episodes[(size_t)target];
    std::string nextTitle = next.grandparentTitle.empty()
                                ? fmt::format("S{}E{} — {}", next.parentIndex, next.index, next.title)
                                : fmt::format("{} · S{}E{} — {}", next.grandparentTitle, next.parentIndex,
                                      next.index, next.title);
    std::string text = fmt::format("main/stremio/playback/next_in"_i18n, remaining, nextTitle);

    if (this->upNextLabel) {
        this->upNextLabel->setText(text);
        return;
    }

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(760);
    content->setPadding(20, 20, 20, 20);
    auto* label = new brls::Label();
    label->setSingleLine(false);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setFontSize(22);
    label->setText(text);
    content->addView(label);

    auto* dialog = new brls::Dialog(content);
    this->upNextDialog = dialog;
    this->upNextLabel = label;
    dialog->addButton("main/stremio/playback/play_next"_i18n, [this]() {
        this->upNextDialog = nullptr;
        this->upNextLabel = nullptr;
        this->view->playNext(1);
    });
    dialog->addButton("main/stremio/playback/cancel_autoplay"_i18n, [this]() {
        this->upNextDialog = nullptr;
        this->upNextLabel = nullptr;
        this->upNextDismissed = true;
        this->view->setAutoNext(false);
    });
    if (!this->item.grandparentRatingKey.empty()) {
        dialog->addButton("main/stremio/playback/open_series"_i18n, [this]() {
            this->upNextDialog = nullptr;
            this->upNextLabel = nullptr;
            media::Item show;
            show.ratingKey = this->item.grandparentRatingKey;
            show.type = media::mediaTypeShow;
            show.title = this->item.grandparentTitle;
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [show]() {
                if (auto* focus = brls::Application::getCurrentFocus())
                    ui::presentDetail(focus, new MediaSeries(show));
            });
        });
    }
    dialog->open();
}

void PlayerView::playMedia(const int64_t seekMs) {
    // Capture/automation guard: in GMCA_NAV_PIPE mode a stray "Play" from the
    // screenshot harness must never actually start playback — doing so pushes a
    // watch-progress report to the server and pollutes Continue Watching. Bail
    // out immediately (the empty player pops itself, leaving us on the detail).
    if (std::getenv("GMCA_NAV_PIPE")) { VideoView::close(); return; }

    const uint64_t generation = ++this->playbackGeneration;

    // Release any transcode session we were running before (re)loading. Covers
    // quality/track switches, episode navigation, and transcode->direct play.
    // Without it each reload orphaned a server-side session (verified on dev:
    // they stack up at ~0% progress and never free), starving new transcodes.
    // deliberate (re)start: allow the direct-play fallback to trigger again
    this->resolvingRecoverySources = false;

    // Fast path: the caller already resolved the exact source (Stremio source
    // picker passes the fully-resolved item + chosen index). Re-fetching would
    // re-resolve streams and could return a different order/set, silently playing
    // a different release than the one selected — so play the chosen one directly.
    {
        auto accessible = [](const media::Media& m) {
            for (auto& p : m.parts)
                if (p.accessible && p.exists && !p.key.empty()) return true;
            return false;
        };
        if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
            accessible(this->item.media[this->preferredVersion])) {
            this->stream = this->item.media[this->preferredVersion];
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
            return;
        }
    }

    ASYNC_RETAIN
    // fresh metadata: Media/Part/Stream + chapters
    auto detailReady = [ASYNC_TOKEN, seekMs, generation](const media::Item& item) {
            ASYNC_RELEASE
            if (generation != this->playbackGeneration) return;
            this->item = item;

            // caller-chosen source (Stremio picker) if it still resolves to an
            // accessible file; otherwise the first accessible version.
            const media::Media* chosen = nullptr;
            int chosenIndex = -1;
            if (AppConfig::instance().backend().type() == media::BackendType::Stremio && seekMs > 0 &&
                this->preferredVersion < 0)
                this->preferredVersion = stremio::savedPlaybackSource(this->item);
            auto accessible = [](const media::Media& m) {
                for (auto& p : m.parts)
                    if (p.accessible && p.exists && !p.key.empty()) return true;
                return false;
            };
            if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
                accessible(this->item.media[this->preferredVersion])) {
                chosenIndex = this->preferredVersion;
                chosen = &this->item.media[(size_t)chosenIndex];
            }
            for (size_t i = 0; i < this->item.media.size() && !chosen; ++i) {
                if (!accessible(this->item.media[i])) continue;
                chosenIndex = (int)i;
                chosen = &this->item.media[i];
            }
            if (!chosen) {
                Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                return;
            }
            this->preferredVersion = chosenIndex;
            this->stream = *chosen;
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
        };
    auto detailError = [ASYNC_TOKEN, generation](const std::string& ex) {
        ASYNC_RELEASE
        if (generation != this->playbackGeneration) return;
        Dialog::show(ex, []() { VideoView::close(); });
    };
    if (auto* backend = dynamic_cast<stremio::StremioBackend*>(&AppConfig::instance().backend()))
        backend->getResumeDetail(this->itemId, seekMs > 0, detailReady, detailError);
    else
        AppConfig::instance().backend().getItemDetail(this->itemId, true, detailReady, detailError);
}

void PlayerView::startPlayback(const int64_t seekMs, bool forceDirect) {
    stremio::archive::Cache::instance().playbackState(archivePlaybackSession, false);
    // We are about to (re)load: mpv will drop any sub-add'ed tracks. Clear the
    // loaded flag so a subtitle fetch landing mid-load waits for MPV_LOADED to
    // re-add. (External subtitles are resolved AFTER the playback task is queued
    // — see the note at the end of this function.)
    this->mpvLoaded = false;
    this->playbackCheckpoint.begin(seekMs);
    if (AppConfig::instance().backend().type() == media::BackendType::Stremio) {
        stremio::beginPlayback(this->itemId, this->sessionId);
        stremio::rememberPlayback(this->item, this->stream, seekMs, this->item.duration);
    }

    media::PlaybackOptions opts;
    opts.seekMs = seekMs;
    opts.bitrateCap = MPVCore::VIDEO_QUALITY;
    // forceDirect: the transcode->direct-play fallback re-resolves with direct
    // play forced (resolvePlayback returns the direct source when set).
    opts.forceDirectPlay = MPVCore::FORCE_DIRECTPLAY || forceDirect;
    opts.audioStreamId = PlayerSetting::selectedAudio;
    opts.subtitleStreamId = PlayerSetting::selectedSubtitle;
    opts.burnSubtitles = PlayerSetting::selectedSubtitle > 0;
    opts.videoCodec = "h264";
    opts.sessionId = this->sessionId;

    // copies for the worker thread (avoids racing on this->item during a switch)
    media::Item item = this->item;
    media::Media version = this->stream;
    const uint64_t generation = this->playbackGeneration;

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, item, version, opts, generation]() {
        try {
            stremio::archive::Cache::instance().waitForPlayback();
            media::PlaybackSource src = AppConfig::instance().backend().resolvePlayback(item, version, opts);
            brls::sync([ASYNC_TOKEN, src, generation]() {
                ASYNC_RELEASE
                if (generation != this->playbackGeneration) return;
                // A backend may report "nothing playable" with an empty url
                // (e.g. Stremio with no direct/debrid stream) instead of throwing
                // across the async/TU boundary; surface it as a player error.
                if (src.url.empty()) {
                    if (this->trySourceRecovery()) return;
                    Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                    return;
                }
                this->playMethod = src.playMethod;
                MPVCore::instance().setUrl(src.url, src.mpvExtra);
                // Resolve subtitles only after a playable URL has actually been
                // accepted. This preserves the single-worker playback-first order
                // while avoiding an addon subtitle request for a failed source.
                this->resolveExternalSubtitles();
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            brls::sync([ASYNC_TOKEN, msg, generation]() {
                ASYNC_RELEASE
                if (generation != this->playbackGeneration) return;
                if (this->trySourceRecovery()) return;
                Dialog::show(msg, []() { VideoView::close(); });
            });
        }
    });

}

void PlayerView::resolveExternalSubtitles() {
    // Subtitle providers may use physical-file hints from the selected source.
    // Cache per item+source, not only per video, so changing Stremio release can
    // legitimately re-resolve a different subtitle set.
    std::string key = this->item.ratingKey;
    if (!this->stream.parts.empty()) key += "\n" + this->stream.parts.front().key;
    if (key.empty() || key == this->externalSubsItem) return;
    this->externalSubsItem = key;
    this->externalSubs.clear();  // drop the previous source's subs before the switch lands

    ASYNC_RETAIN
    AppConfig::instance().backend().getSubtitles(
        this->item, this->stream,
        [ASYNC_TOKEN, key](std::vector<media::Stream> subs) {
            ASYNC_RELEASE
            // a newer switch superseded this fetch -> its result is stale
            if (key != this->externalSubsItem) return;
            this->externalSubs = std::move(subs);
            // if the file is already playing, add now; otherwise MPV_LOADED will
            if (this->mpvLoaded) this->addExternalSubtitles();
        },
        [ASYNC_TOKEN, key](const std::string&) {
            ASYNC_RELEASE
            // resolution failed (offline / addon error): leave the set empty, the
            // player still plays; no dialog (subtitles are best-effort).
        });
}

void PlayerView::addExternalSubtitles() {
    if (this->externalSubs.empty()) return;
    auto& mpv = MPVCore::instance();
    auto& backend = AppConfig::instance().backend();

    // Preferred language: "auto" follows the app locale, "off" disables auto-
    // selection, otherwise an explicit 2-letter code (PLAYER_SUBTITLE_LANG).
    std::string pref = AppConfig::instance().getItem(AppConfig::PLAYER_SUBTITLE_LANG, std::string("auto"));
    if (pref == "auto") {
        std::string loc = brls::Application::getLocale();  // "es", "en-US", "zh-Hans"...
        pref = loc.substr(0, loc.find('-'));
    } else if (pref == "off") {
        pref.clear();
    }

    bool selectedPreferred = false;
    for (auto& s : this->externalSubs) {
        if (s.key.empty()) continue;
        std::string url = backend.subtitleSidecarUrl(s.key);
        // select the track matching the preferred language; add the rest as
        // "auto" so they stay pickable in the subtitle menu without stealing it.
        bool preferred = !selectedPreferred && !pref.empty() && s.languageTag == pref;
        const char* flag = preferred ? "select" : "auto";
#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
        auto safety = ps4SubtitleSidecarSafety(url);
        if (safety == Ps4SubtitleSidecarSafety::Risky)
            brls::Logger::info("PS4 subtitle guard: bitmap external sidecar is manual-select only {}", url);
        // Preserve preferred-language auto-selection for known-safe text formats
        // only. Bitmap/unknown URLs stay available but require explicit selection.
        if (safety != Ps4SubtitleSidecarSafety::SafeText) flag = "auto";
#endif
        if (std::strcmp(flag, "select") == 0) selectedPreferred = true;
        mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str(), s.languageTag.c_str());
    }
}

bool PlayerView::trySourceRecovery(int64_t resumeMs) {
    auto* backend = dynamic_cast<stremio::StremioBackend*>(&AppConfig::instance().backend());
    if (!backend) return false;
    if (!this->item.sourcesComplete) {
        if (this->resolvingRecoverySources) return true;
        this->resolvingRecoverySources = true;
        const auto generation = this->playbackGeneration;
        const int64_t resume = resumeMs < 0 ? this->playbackCheckpoint.position() : resumeMs;
        ASYNC_RETAIN
        backend->completePlaybackSources(this->item,
            [ASYNC_TOKEN, generation, resume](const media::Item& detail) {
                ASYNC_RELEASE
                if (generation != this->playbackGeneration) return;
                this->resolvingRecoverySources = false;
                this->item = detail;
                this->preferredVersion = -1; // locate the failed source by identity below
                if (!this->trySourceRecovery(resume)) Dialog::show("main/player/error"_i18n);
            },
            [ASYNC_TOKEN, generation](const std::string& error) {
                ASYNC_RELEASE
                if (generation != this->playbackGeneration) return;
                this->resolvingRecoverySources = false;
                Dialog::show(error);
            });
        return true;
    }
    if (this->item.media.size() < 2) return false;

    auto accessible = [](const media::Media& m) {
        for (const auto& p : m.parts)
            if (p.accessible && p.exists && !p.key.empty()) return true;
        return false;
    };
    auto mediaKey = [](const media::Media& m) -> std::string {
        return !m.sourceIdentity.empty() ? m.sourceIdentity :
            (m.parts.empty() ? std::string() : m.parts.front().key);
    };

    int current = this->preferredVersion;
    if (current < 0 || current >= (int)this->item.media.size()) {
        std::string key = mediaKey(this->stream);
        for (size_t i = 0; i < this->item.media.size(); ++i)
            if (!key.empty() && mediaKey(this->item.media[i]) == key) {
                current = (int)i;
                break;
            }
    }

    std::vector<int> alternatives;
    std::vector<std::string> labels;
    for (size_t i = 0; i < this->item.media.size(); ++i) {
        if ((int)i == current || !accessible(this->item.media[i])) continue;
        const auto& m = this->item.media[i];
        std::string label;
        if (!m.videoResolution.empty()) label += m.videoResolution;
        if (!m.videoCodec.empty()) label += (label.empty() ? "" : " · ") + m.videoCodec;
        if (!m.label.empty()) label += (label.empty() ? "" : " · ") + m.label;
        if (!m.detail.empty()) label += (label.empty() ? "" : " · ") + m.detail;
        if (label.empty()) label = fmt::format("main/stremio/playback/source_number"_i18n, i + 1);
        alternatives.push_back((int)i);
        labels.push_back(std::move(label));
    }
    if (alternatives.empty()) return false;

    if (resumeMs < 0) {
        resumeMs = this->playbackCheckpoint.position();
    }
    const int64_t resume = resumeMs;
    auto* picker = new brls::Dropdown("main/stremio/playback/choose_another"_i18n, labels,
        [this, alternatives, resume](int selected) {
            if (selected < 0 || selected >= (int)alternatives.size()) return;
            int index = alternatives[(size_t)selected];
            MPVCore::instance().reset();
            this->preferredVersion = index;
            this->stream = this->item.media[(size_t)index];
            this->externalSubsItem.clear();
            this->externalSubs.clear();
            this->startPlayback(resume);
        });
    brls::Application::pushActivity(new brls::Activity(picker));
    return true;
}


void PlayerView::checkpointPlayback(int64_t timeMs) {
    if (timeMs < 0) return;
    if (AppConfig::instance().backend().type() == media::BackendType::Stremio &&
        !this->playbackCheckpoint.observe(timeMs)) return;
    if (AppConfig::instance().backend().type() != media::BackendType::Stremio || this->scrobbled) return;
    const int64_t duration = MPVCore::instance().duration > 0
        ? MPVCore::instance().duration * 1000 : this->item.duration;
    if (duration > 0) this->item.duration = duration;
    stremio::rememberPlayback(this->item, this->stream, timeMs, duration);
}

void PlayerView::reportTimeline(const std::string& state, int64_t timeMs) {
    if (AppConfig::instance().backend().type() == media::BackendType::Stremio) {
        if (!this->playbackCheckpoint.ready() && state != "stopped") return;
        if (state == "paused" && this->playbackCheckpoint.ready()) this->checkpointPlayback(timeMs);
        if (state != "stopped") timeMs = this->playbackCheckpoint.position();
    }
    media::PlayState st = state == "paused"    ? media::PlayState::Paused
                          : state == "stopped" ? media::PlayState::Stopped
                                               : media::PlayState::Playing;
    AppConfig::instance().backend().reportProgress(this->itemId, st, timeMs, this->item.duration, this->sessionId);
    if (AppConfig::instance().backend().type() == media::BackendType::Stremio) {
        const bool completed = stremio::playbackCompleted(this->itemId);
        this->scrobbled = completed;
        // Reflect the backend decision in the loaded navigation list; returning
        // to an earlier episode must not offer an episode just completed here.
        if (this->episodeIndex >= 0 && this->episodeIndex < (int)this->episodes.size()) {
            auto& episode = this->episodes[(size_t)this->episodeIndex];
            episode.viewCount = completed ? 1 : 0;
            episode.viewOffset = completed ? 0 : std::max<int64_t>(0, timeMs);
            episode.duration = this->item.duration;
        }
    }
}

void PlayerView::reportStop(int64_t timeMs) {
    if (timeMs < 0) {
        if (this->playbackCheckpoint.ready())
            this->checkpointPlayback(int64_t(MPVCore::instance().getDouble("playback-time", -1) * 1000));
        timeMs = AppConfig::instance().backend().type() == media::BackendType::Stremio
            ? this->playbackCheckpoint.position() : int64_t(MPVCore::instance().playback_time) * 1000;
    }
    this->reportTimeline("stopped", timeMs);
    brls::Logger::debug("PlayerView reportStop {}", this->sessionId);
}

bool PlayerView::toggleQuality() {
    std::vector<std::string> options = {"main/player/auto"_i18n};
    std::vector<int64_t> values = {0};
    int64_t videoBitRate = this->stream.bitrate * 1000;  // compatible media field

    if (videoBitRate >= 15000000) options.push_back("20 Mbps"), values.push_back(20000000);
    if (videoBitRate >= 10000000) options.push_back("15 Mbps"), values.push_back(15000000);
    if (videoBitRate >= 8000000) options.push_back("10 Mbps"), values.push_back(10000000);
    if (videoBitRate >= 6000000) options.push_back("8 Mbps"), values.push_back(8000000);
    if (videoBitRate >= 4000000) options.push_back("6 Mbps"), values.push_back(6000000);
    if (videoBitRate >= 3000000) options.push_back("4 Mbps"), values.push_back(4000000);
    if (videoBitRate >= 1500000) options.push_back("3 Mbps"), values.push_back(3000000);
    if (videoBitRate >= 720000) options.push_back("1.5 Mbps"), values.push_back(1500000);
    options.push_back("720 kbps"), values.push_back(720000);
    options.push_back("420 kbps"), values.push_back(420000);

    auto it = std::find(values.begin(), values.end(), MPVCore::VIDEO_QUALITY);
    if (it == values.end()) it = values.begin();

    brls::Dropdown* dropdown = new brls::Dropdown(
        "main/player/quality"_i18n, options,
        [values](int selected) {
            MPVCore::VIDEO_QUALITY = values[selected];
            AppConfig::instance().setItem(AppConfig::PLAYER_VIDEO_QUALITY, MPVCore::VIDEO_QUALITY);
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
            return true;
        },
        std::distance(values.begin(), it));

    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}
