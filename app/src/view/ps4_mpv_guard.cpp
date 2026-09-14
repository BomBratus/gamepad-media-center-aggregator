#define GMCA_PS4_MPV_GUARD_IMPLEMENTATION
#include <mpv/render_gl.h>

#ifdef __PS4__

#include "utils/config.hpp"

#include <borealis.hpp>
#include <fmt/format.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {

constexpr int64_t WATCHDOG_SAMPLE_MS = 1000;
constexpr int64_t STALLED_FRAME_MS = 3500;
constexpr int64_t RECOVERY_COOLDOWN_MS = 6000;
constexpr int64_t STABLE_RESET_MS = 10000;
constexpr int64_t HEARTBEAT_MS = 15000;
constexpr std::streamoff MAX_DIAGNOSTIC_LOG = 2 * 1024 * 1024;

struct GuardContext {
    mpv_render_context *real = nullptr;
    mpv_handle *mpv = nullptr;
    mpv_opengl_init_params gl_init{};
    bool has_gl_init = false;

    mpv_render_update_fn update_callback = nullptr;
    void *update_callback_ctx = nullptr;

    uint64_t update_calls = 0;
    uint64_t frame_updates = 0;
    uint64_t render_calls = 0;
    uint64_t swap_calls = 0;

    int64_t last_frame_update_ms = 0;
    int64_t last_render_ms = 0;
    int64_t last_recovery_ms = 0;
    int64_t last_watchdog_sample_ms = 0;
    int64_t last_reconfig_ms = 0;
    int64_t last_heartbeat_ms = 0;
    double last_watchdog_playback = 0.0;

    int recovery_stage = 0;  // 0=healthy, 1=video-reload attempted, 2=context rebuilt
    int reconfig_count = 0;
    int gl_error_count = 0;

    int last_fbo = -1;
    int last_fbo_w = 0;
    int last_fbo_h = 0;
    int last_fbo_format = 0;

    bool hard_recovery_pending = false;
    std::string hard_recovery_reason;
};

GuardContext *g_active_guard = nullptr;
std::mutex g_log_mutex;

int64_t monotonicMs() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

std::string flatten(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

std::string diagnosticDir() {
    return AppConfig::instance().configDir();
}

std::string diagnosticPath() {
    return diagnosticDir() + "/ps4-video-diagnostics.log";
}

void prepareDiagnosticLog() {
    static bool prepared = false;
    if (prepared) return;
    prepared = true;

    const std::string dir = diagnosticDir();
    mkdir(dir.c_str(), 0777);

    const std::string path = diagnosticPath();
    std::ifstream existing(path, std::ios::binary | std::ios::ate);
    if (existing.is_open()) {
        const auto size = existing.tellg();
        if (size != std::streampos(-1) && size > std::streampos(MAX_DIAGNOSTIC_LOG)) {
            existing.close();
            const std::string oldPath = path + ".old";
            std::remove(oldPath.c_str());
            std::rename(path.c_str(), oldPath.c_str());
        }
    }
}

void diagnostic(const std::string &message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    prepareDiagnosticLog();
    std::ofstream out(diagnosticPath(), std::ios::app);
    if (!out.is_open()) return;
    out << "[" << monotonicMs() << " ms] " << message << '\n';
}

std::string propString(mpv_handle *mpv, const char *name) {
    char *value = nullptr;
    if (!mpv || mpv_get_property(mpv, name, MPV_FORMAT_STRING, &value) < 0 || !value) return "-";
    std::string result = flatten(value);
    mpv_free(value);
    return result.empty() ? "-" : result;
}

int64_t propInt(mpv_handle *mpv, const char *name, int64_t fallback = -1) {
    int64_t value = fallback;
    if (!mpv || mpv_get_property(mpv, name, MPV_FORMAT_INT64, &value) < 0) return fallback;
    return value;
}

int propFlag(mpv_handle *mpv, const char *name, int fallback = -1) {
    int value = fallback;
    if (!mpv || mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &value) < 0) return fallback;
    return value;
}

double propDouble(mpv_handle *mpv, const char *name, double fallback = -1.0) {
    double value = fallback;
    if (!mpv || mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &value) < 0) return fallback;
    return value;
}

std::string glString(GLenum name) {
    const GLubyte *value = glGetString(name);
    return value ? flatten(reinterpret_cast<const char *>(value)) : "-";
}

GuardContext *guardFrom(mpv_render_context *ctx) {
    auto *candidate = reinterpret_cast<GuardContext *>(ctx);
    return candidate == g_active_guard ? candidate : nullptr;
}

std::string snapshot(GuardContext *g, const char *reason) {
    if (!g || !g->mpv) return fmt::format("reason={} no-active-player", reason);

    const int64_t now = monotonicMs();
    const int64_t frameAge = g->last_frame_update_ms > 0 ? now - g->last_frame_update_ms : -1;
    const int64_t renderAge = g->last_render_ms > 0 ? now - g->last_render_ms : -1;

    return fmt::format(
        "reason={} pos={:.3f} apts={:.3f} vpts={:.3f} avsync={:.4f} "
        "codec={} format={} pixfmt={} size={}x{} fps(container/estimated)={:.3f}/{:.3f} "
        "color(primaries/gamma/levels)={}/{}/{} hwdec={} vo={} pause={} cache_pause={} core_idle={} "
        "drops(decoder/frame/mistimed)={}/{}/{} cache={:.3f}s "
        "fbo(id/size/format)={}/{}x{}/{} "
        "guard(update_calls/frame_updates/renders/swaps)={}/{}/{}/{} ages(frame/render)={}/{}ms "
        "reconfigs={} stage={} gl_errors={}",
        reason,
        propDouble(g->mpv, "playback-time"),
        propDouble(g->mpv, "audio-pts"),
        propDouble(g->mpv, "video-pts"),
        propDouble(g->mpv, "avsync"),
        propString(g->mpv, "video-codec"),
        propString(g->mpv, "video-format"),
        propString(g->mpv, "video-params/pixelformat"),
        propInt(g->mpv, "video-params/w"),
        propInt(g->mpv, "video-params/h"),
        propDouble(g->mpv, "container-fps"),
        propDouble(g->mpv, "estimated-vf-fps"),
        propString(g->mpv, "video-params/primaries"),
        propString(g->mpv, "video-params/gamma"),
        propString(g->mpv, "video-params/colorlevels"),
        propString(g->mpv, "hwdec-current"),
        propFlag(g->mpv, "vo-configured"),
        propFlag(g->mpv, "pause"),
        propFlag(g->mpv, "paused-for-cache"),
        propFlag(g->mpv, "core-idle"),
        propInt(g->mpv, "decoder-frame-drop-count"),
        propInt(g->mpv, "frame-drop-count"),
        propInt(g->mpv, "mistimed-frame-count"),
        propDouble(g->mpv, "demuxer-cache-duration"),
        g->last_fbo,
        g->last_fbo_w,
        g->last_fbo_h,
        g->last_fbo_format,
        g->update_calls,
        g->frame_updates,
        g->render_calls,
        g->swap_calls,
        frameAge,
        renderAge,
        g->reconfig_count,
        g->recovery_stage,
        g->gl_error_count);
}

void logSnapshot(GuardContext *g, const char *reason) {
    diagnostic(snapshot(g, reason));
}

void resetPlaybackWatchdog(GuardContext *g) {
    if (!g) return;
    const int64_t now = monotonicMs();
    g->last_frame_update_ms = now;
    g->last_render_ms = now;
    g->last_recovery_ms = 0;
    g->last_watchdog_sample_ms = 0;
    g->last_watchdog_playback = 0.0;
    g->last_reconfig_ms = 0;
    g->last_heartbeat_ms = now;
    g->recovery_stage = 0;
    g->reconfig_count = 0;
    g->hard_recovery_pending = false;
    g->hard_recovery_reason.clear();
}

int requestVideoReload(GuardContext *g, const char *reason) {
    if (!g || !g->mpv) return MPV_ERROR_INVALID_PARAMETER;
    const char *command[] = {"video-reload", nullptr};
    const int result = mpv_command_async(g->mpv, 0, command);
    diagnostic(fmt::format("recovery=video-reload reason={} result={} ({})", reason, result,
        result < 0 ? mpv_error_string(result) : "ok"));
    return result;
}

bool recreateRealContext(GuardContext *g, const char *reason) {
    if (!g || !g->mpv || !g->has_gl_init) {
        diagnostic(fmt::format("recovery=context-recreate reason={} skipped=no-gl-init", reason));
        return false;
    }

    const int64_t now = monotonicMs();
    if (g->last_recovery_ms > 0 && now - g->last_recovery_ms < RECOVERY_COOLDOWN_MS) return false;
    g->last_recovery_ms = now;

    logSnapshot(g, "before-context-recreate");

    if (g->real) {
        mpv_render_context_set_update_callback(g->real, nullptr, nullptr);
        glFinish();
        mpv_render_context_free(g->real);
        g->real = nullptr;
    }

    mpv_opengl_init_params glInit = g->gl_init;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    const int result = mpv_render_context_create(&g->real, g->mpv, params);
    if (result < 0 || !g->real) {
        diagnostic(fmt::format("recovery=context-recreate reason={} FAILED result={} ({})", reason, result,
            result < 0 ? mpv_error_string(result) : "null context"));
        g->hard_recovery_pending = true;
        g->hard_recovery_reason = "retry-after-create-failure";
        return false;
    }

    if (g->update_callback) {
        mpv_render_context_set_update_callback(g->real, g->update_callback, g->update_callback_ctx);
    }

    g->last_frame_update_ms = now;
    g->last_render_ms = now;
    g->recovery_stage = 2;
    g->hard_recovery_pending = false;
    g->hard_recovery_reason.clear();

    diagnostic(fmt::format("recovery=context-recreate reason={} result=ok", reason));
    brls::Logger::warning("PS4 video: recreated libmpv render context ({})", reason);
    return true;
}

void performHardRecoveryIfRequested(GuardContext *g) {
    if (!g || !g->hard_recovery_pending) return;
    const int64_t now = monotonicMs();
    if (g->last_recovery_ms > 0 && now - g->last_recovery_ms < RECOVERY_COOLDOWN_MS) return;

    const std::string reason = g->hard_recovery_reason.empty() ? "deferred" : g->hard_recovery_reason;
    logSnapshot(g, reason.c_str());
    if (recreateRealContext(g, reason.c_str())) {
        requestVideoReload(g, "after-context-recreate");
    }
}

void watchdogPlayback(GuardContext *g, double playbackTime) {
    if (!g || !g->mpv) return;

    const int64_t now = monotonicMs();
    if (g->last_watchdog_sample_ms == 0) {
        g->last_watchdog_sample_ms = now;
        g->last_watchdog_playback = playbackTime;
        return;
    }
    if (now - g->last_watchdog_sample_ms < WATCHDOG_SAMPLE_MS) return;

    const double advanced = playbackTime - g->last_watchdog_playback;
    const int64_t frameAge = g->last_frame_update_ms > 0 ? now - g->last_frame_update_ms : STALLED_FRAME_MS + 1;

    // Large jumps are seeks, not evidence of a stalled decoder/renderer.
    if (advanced < -0.25 || advanced > 10.0) {
        g->last_watchdog_sample_ms = now;
        g->last_watchdog_playback = playbackTime;
        g->last_frame_update_ms = now;
        return;
    }

    const int paused = propFlag(g->mpv, "pause", 0);
    const int cachePaused = propFlag(g->mpv, "paused-for-cache", 0);
    const int voConfigured = propFlag(g->mpv, "vo-configured", -1);
    const std::string codec = propString(g->mpv, "video-codec");
    const bool hasVideo = codec != "-";
    const bool playbackAdvancing = advanced >= 0.50;
    const bool suspiciousVo = hasVideo && voConfigured == 0;
    const bool noFreshVideoFrame = hasVideo && frameAge > STALLED_FRAME_MS;

    if (playbackAdvancing && now - g->last_heartbeat_ms >= HEARTBEAT_MS) {
        logSnapshot(g, "HEARTBEAT");
        g->last_heartbeat_ms = now;
    }

    if (!paused && !cachePaused && playbackAdvancing && (suspiciousVo || noFreshVideoFrame)) {
        const bool cooldownExpired = g->last_recovery_ms == 0 || now - g->last_recovery_ms >= RECOVERY_COOLDOWN_MS;
        if (cooldownExpired) {
            logSnapshot(g, suspiciousVo ? "watchdog-vo-not-configured" : "watchdog-no-video-frames");
            if (g->recovery_stage == 0) {
                brls::Logger::warning("PS4 video: watchdog detected stalled video; reloading video track");
                requestVideoReload(g, suspiciousVo ? "vo-not-configured" : "no-fresh-frame");
                g->recovery_stage = 1;
                g->last_recovery_ms = now;
                g->last_frame_update_ms = now;  // grace period for the reload
            } else {
                g->hard_recovery_pending = true;
                g->hard_recovery_reason = suspiciousVo ? "repeated-vo-not-configured" : "repeated-no-video-frames";
            }
        }
    } else if (g->recovery_stage > 0 && playbackAdvancing && frameAge < 1200 &&
               now - g->last_recovery_ms > STABLE_RESET_MS) {
        diagnostic("watchdog=stable-video recovery-stage-reset");
        g->recovery_stage = 0;
    }

    g->last_watchdog_sample_ms = now;
    g->last_watchdog_playback = playbackTime;
}

void inspectEvent(GuardContext *g, mpv_event *event) {
    if (!g || !event) return;
    const int64_t now = monotonicMs();

    switch (event->event_id) {
    case MPV_EVENT_START_FILE:
        resetPlaybackWatchdog(g);
        diagnostic("event=START_FILE");
        break;
    case MPV_EVENT_FILE_LOADED:
        logSnapshot(g, "FILE_LOADED");
        break;
    case MPV_EVENT_VIDEO_RECONFIG:
        g->reconfig_count++;
        g->last_reconfig_ms = now;
        // A reconfigure legitimately pauses frame delivery briefly. Give it a
        // fresh grace window, then let the normal watchdog decide whether the
        // decoder/VO actually came back.
        g->last_frame_update_ms = now;
        logSnapshot(g, "VIDEO_RECONFIG");
        glFinish();
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        logSnapshot(g, "PLAYBACK_RESTART");
        break;
    case MPV_EVENT_SEEK:
        g->last_frame_update_ms = now;
        g->last_watchdog_sample_ms = 0;
        diagnostic("event=SEEK watchdog-grace-reset");
        break;
    case MPV_EVENT_END_FILE: {
        auto *end = static_cast<mpv_event_end_file *>(event->data);
        diagnostic(fmt::format("event=END_FILE reason={} error={} ({})", end ? int(end->reason) : -1,
            end ? end->error : 0, end && end->error < 0 ? mpv_error_string(end->error) : "none"));
        logSnapshot(g, "END_FILE");
        g->last_watchdog_sample_ms = 0;
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        auto *prop = static_cast<mpv_event_property *>(event->data);
        if (prop && prop->name && std::strcmp(prop->name, "playback-time") == 0 &&
            prop->format == MPV_FORMAT_DOUBLE && prop->data) {
            watchdogPlayback(g, *static_cast<double *>(prop->data));
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace

int gmca_ps4_mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params) {
    if (!res) return MPV_ERROR_INVALID_PARAMETER;

    mpv_render_context *real = nullptr;
    const int result = mpv_render_context_create(&real, mpv, params);
    if (result < 0 || !real) return result < 0 ? result : MPV_ERROR_UNINITIALIZED;

    auto *guard = new GuardContext();
    guard->real = real;
    guard->mpv = mpv;
    guard->last_frame_update_ms = monotonicMs();
    guard->last_render_ms = guard->last_frame_update_ms;
    guard->last_heartbeat_ms = guard->last_frame_update_ms;

    if (params) {
        for (mpv_render_param *param = params; param->type != MPV_RENDER_PARAM_INVALID; ++param) {
            if (param->type == MPV_RENDER_PARAM_OPENGL_INIT_PARAMS && param->data) {
                guard->gl_init = *static_cast<mpv_opengl_init_params *>(param->data);
                guard->has_gl_init = true;
            }
        }
    }

    g_active_guard = guard;
    *res = reinterpret_cast<mpv_render_context *>(guard);

    prepareDiagnosticLog();
    diagnostic(fmt::format(
        "guard=active gl_init={} log={} gl_vendor={} gl_renderer={} gl_version={} mpv={} ffmpeg={}",
        guard->has_gl_init ? "yes" : "no",
        diagnosticPath(),
        glString(GL_VENDOR),
        glString(GL_RENDERER),
        glString(GL_VERSION),
        propString(mpv, "mpv-version"),
        propString(mpv, "ffmpeg-version")));
    brls::Logger::info("PS4 video guard active; diagnostics: {}", diagnosticPath());
    return 0;
}

void gmca_ps4_mpv_render_context_set_update_callback(
    mpv_render_context *ctx, mpv_render_update_fn callback, void *callback_ctx) {
    GuardContext *guard = guardFrom(ctx);
    if (!guard) {
        mpv_render_context_set_update_callback(ctx, callback, callback_ctx);
        return;
    }

    guard->update_callback = callback;
    guard->update_callback_ctx = callback_ctx;
    if (guard->real) mpv_render_context_set_update_callback(guard->real, callback, callback_ctx);
}

uint64_t gmca_ps4_mpv_render_context_update(mpv_render_context *ctx) {
    GuardContext *guard = guardFrom(ctx);
    if (!guard) return mpv_render_context_update(ctx);
    if (!guard->real) return 0;

    const uint64_t flags = mpv_render_context_update(guard->real);
    guard->update_calls++;
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        guard->frame_updates++;
        guard->last_frame_update_ms = monotonicMs();
    }
    return flags;
}

int gmca_ps4_mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params) {
    GuardContext *guard = guardFrom(ctx);
    if (!guard) return mpv_render_context_render(ctx, params);
    if (!guard->real) return MPV_ERROR_UNINITIALIZED;

    if (params) {
        for (mpv_render_param *param = params; param->type != MPV_RENDER_PARAM_INVALID; ++param) {
            if (param->type == MPV_RENDER_PARAM_OPENGL_FBO && param->data) {
                auto *fbo = static_cast<mpv_opengl_fbo *>(param->data);
                guard->last_fbo = fbo->fbo;
                guard->last_fbo_w = fbo->w;
                guard->last_fbo_h = fbo->h;
                guard->last_fbo_format = fbo->internal_format;
                break;
            }
        }
    }

    // Drain pre-existing GL errors so an error left by NanoVG/UI rendering is
    // not blamed on libmpv. Bound the loop in case a broken driver keeps
    // returning an error forever.
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) break;
    }

    const int result = mpv_render_context_render(guard->real, params);

    // PS4's GLES path is deliberately serialized. Switchfin already needed a
    // similar glFinish workaround on another constrained GL backend; here it
    // also prevents the next NanoVG frame from racing unfinished video work.
    glFinish();
    const GLenum glError = glGetError();

    guard->render_calls++;
    guard->last_render_ms = monotonicMs();

    if (result < 0 || glError != GL_NO_ERROR) {
        guard->gl_error_count++;
        if (!guard->hard_recovery_pending) {
            guard->hard_recovery_pending = true;
            guard->hard_recovery_reason = result < 0 ? "mpv-render-error" : "gl-error-after-render";
            brls::Logger::warning("PS4 video render error: mpv={} gl=0x{:x}; recovery queued", result,
                static_cast<unsigned int>(glError));
            diagnostic(fmt::format("render-error mpv={} ({}) gl=0x{:x}", result,
                result < 0 ? mpv_error_string(result) : "ok", static_cast<unsigned int>(glError)));
        }
    }

    return result;
}

void gmca_ps4_mpv_render_context_report_swap(mpv_render_context *ctx) {
    GuardContext *guard = guardFrom(ctx);
    if (!guard) {
        mpv_render_context_report_swap(ctx);
        return;
    }
    if (!guard->real) return;
    mpv_render_context_report_swap(guard->real);
    guard->swap_calls++;
}

void gmca_ps4_mpv_render_context_free(mpv_render_context *ctx) {
    GuardContext *guard = guardFrom(ctx);
    if (!guard) {
        mpv_render_context_free(ctx);
        return;
    }

    logSnapshot(guard, "CONTEXT_FREE");
    if (guard->real) {
        mpv_render_context_set_update_callback(guard->real, nullptr, nullptr);
        glFinish();
        mpv_render_context_free(guard->real);
        guard->real = nullptr;
    }
    g_active_guard = nullptr;
    delete guard;
}

mpv_event *gmca_ps4_mpv_wait_event(mpv_handle *mpv, double timeout) {
    mpv_event *event = mpv_wait_event(mpv, timeout);
    GuardContext *guard = g_active_guard;
    if (guard && guard->mpv == mpv) {
        inspectEvent(guard, event);
        // Only rebuild the real render context once MPVCore has drained the
        // event queue. This keeps the event currently being delivered valid and
        // performs teardown/recreation at the safest point in the Borealis sync
        // phase, outside the UI render call.
        if (!event || event->event_id == MPV_EVENT_NONE) {
            performHardRecoveryIfRequested(guard);
        }
    }
    return event;
}

#endif  // __PS4__
