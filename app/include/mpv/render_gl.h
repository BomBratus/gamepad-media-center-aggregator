#pragma once

// Keep using the toolchain/libmpv header, but interpose the small subset of
// render API calls used by MPVCore on PS4.  The application include directory
// precedes the platform mpv include directory, so this wrapper is picked first.
// On every other platform this is a transparent include_next passthrough.
#include_next <mpv/render_gl.h>

#if defined(__PS4__)
#include <mpv/client.h>

int gmca_ps4_mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params);
void gmca_ps4_mpv_render_context_set_update_callback(
    mpv_render_context *ctx, mpv_render_update_fn callback, void *callback_ctx);
uint64_t gmca_ps4_mpv_render_context_update(mpv_render_context *ctx);
int gmca_ps4_mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params);
void gmca_ps4_mpv_render_context_report_swap(mpv_render_context *ctx);
void gmca_ps4_mpv_render_context_free(mpv_render_context *ctx);
mpv_event *gmca_ps4_mpv_wait_event(mpv_handle *mpv, double timeout);

#ifndef GMCA_PS4_MPV_GUARD_IMPLEMENTATION
#define mpv_render_context_create gmca_ps4_mpv_render_context_create
#define mpv_render_context_set_update_callback gmca_ps4_mpv_render_context_set_update_callback
#define mpv_render_context_update gmca_ps4_mpv_render_context_update
#define mpv_render_context_render gmca_ps4_mpv_render_context_render
#define mpv_render_context_report_swap gmca_ps4_mpv_render_context_report_swap
#define mpv_render_context_free gmca_ps4_mpv_render_context_free
#define mpv_wait_event gmca_ps4_mpv_wait_event
#endif
#endif
