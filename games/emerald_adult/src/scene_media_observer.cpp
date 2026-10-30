#include "scene_media_observer.h"

#include "gba_bus.h"
#include "mod_audio.h"
#include "mod_runtime.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "gallery_media_package.h"
#include "../generated/host_scene_state_address.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace emerald_media {
namespace {
constexpr char kPluginId[] = "pokemon-adult.gallery-media-seam.v1";
constexpr std::uint16_t kHostStateActive = 1u << 0;
constexpr std::uint16_t kHostStateFrameVisible = 1u << 1;
constexpr std::uint16_t kHostStateLooping = 1u << 2;
constexpr std::uint16_t kHostStateGuestMusicEnabled = 1u << 3;
constexpr std::uint16_t kHostStateKnownFlags =
    kHostStateActive | kHostStateFrameVisible | kHostStateLooping |
    kHostStateGuestMusicEnabled;

struct GbaHostSceneState {
    std::uint16_t version;
    std::uint16_t size;
    volatile std::uint16_t sequence;
    std::uint16_t flags;
    std::uint32_t invocation_id;
    std::uint16_t scene_id;
    std::uint16_t media_id;
    std::uint16_t frame_index;
    std::uint16_t player_state;
    std::uint32_t elapsed_vblanks;
    std::uint32_t total_vblanks;
    std::uint32_t loop_count;
};
static_assert(sizeof(GbaHostSceneState) == 32);
static_assert(offsetof(GbaHostSceneState, invocation_id) == 8);
static_assert(offsetof(GbaHostSceneState, elapsed_vblanks) == 20);

GalleryMediaPackage sMedia;
std::mutex sStatusMutex;
bool sEnabled;
bool sActivationAttempted;
bool sPackageLoaded;
bool sGuestStateValid;
bool sGuestSceneActive;
bool sFramePublished;
const char* sFallbackReason = "plugin_not_selected";
GbaHostSceneState sLastObservedState{};
std::uint64_t sLastPublishedEpoch = 0;
std::uint32_t sPublishedFrameCount = 0;
std::uint16_t sLastMediaId = 0xFFFF;
std::uint32_t sLastInvocation = 0;
std::uint32_t sLastElapsedVblanks = 0;
std::uint64_t sLastStateEpoch = ~std::uint64_t{0};
GBAModAudioStream sAudioStream = GBA_MOD_AUDIO_STREAM_INVALID;
bool sStreamPlaying = false;
std::atomic<std::size_t> sAudioCursor{0};
std::atomic<std::size_t> sGuestAudioCursor{0};
std::atomic<bool> sAudioMuted{true};

const char* resolve_package_asset(void*, const char* asset_id) {
    return gba_mod_required_asset_path("adult.gallery-media", asset_id);
}

std::size_t read_media_audio(void*, std::int16_t* dst, std::size_t max_frames) {
    if (!dst || max_frames > GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES) return 0;
    const std::size_t offset = sAudioCursor.fetch_add(max_frames, std::memory_order_relaxed);
    if (sAudioMuted.load(std::memory_order_relaxed)) {
        std::fill_n(dst, max_frames, 0);
        return max_frames;
    }
    return sMedia.read_audio(offset, dst, max_frames, sMedia.loops());
}

void stop_stream() {
    if (sAudioStream != GBA_MOD_AUDIO_STREAM_INVALID && sStreamPlaying)
        gba_mod_audio_stream_stop(sAudioStream);
    if (sAudioStream != GBA_MOD_AUDIO_STREAM_INVALID)
        gba_mod_audio_stream_set_native_gain_percent(sAudioStream, 100);
    sStreamPlaying = false;
}

void reset_media() {
    std::lock_guard<std::mutex> status_lock(sStatusMutex);
    // The runtime retires stream capabilities before reset callbacks.
    sAudioStream = GBA_MOD_AUDIO_STREAM_INVALID;
    sStreamPlaying = false;
    sEnabled = false;
    sActivationAttempted = false;
    sPackageLoaded = false;
    sGuestStateValid = false;
    sGuestSceneActive = false;
    sFramePublished = false;
    sFallbackReason = "plugin_not_selected";
    sLastObservedState = {};
    sLastPublishedEpoch = 0;
    sPublishedFrameCount = 0;
    sLastMediaId = 0xFFFF;
    sLastInvocation = 0;
    sLastElapsedVblanks = 0;
    sAudioMuted.store(true, std::memory_order_relaxed);
    sAudioCursor.store(0, std::memory_order_relaxed);
    sGuestAudioCursor.store(0, std::memory_order_relaxed);
    sLastStateEpoch = ~std::uint64_t{0};
    sMedia.clear();
    gba_mod_clear_foreign_background();
}

void activate_media() {
    std::lock_guard<std::mutex> status_lock(sStatusMutex);
    sActivationAttempted = true;
    std::string media_error;
    const char* media_manifest = gba_mod_required_asset_path(
        "adult.gallery-media", "media-manifest-json");
    bool media_loaded = false;
    try {
        media_loaded = media_manifest && sMedia.load(
            media_manifest, resolve_package_asset, nullptr, &media_error);
    } catch (...) {
        media_error = "not enough memory to load bounded scene media";
    }
    if (!media_loaded) {
        std::fprintf(stderr, "[gallery-media] disabled: %s\n",
                     media_error.empty() ? "committed package manifest is unavailable"
                                         : media_error.c_str());
        sEnabled = false;
        sPackageLoaded = false;
        sFallbackReason = "package_invalid";
        return;
    }
    sPackageLoaded = true;
    sFallbackReason = "guest_scene_inactive";
    std::fprintf(stderr, "[gallery-media] ready: scene=%04x frames=%zu samples=%u\n",
                 sMedia.scene_id(), sMedia.frame_count(), sMedia.total_samples());
    sAudioStream = gba_mod_audio_stream_register_s16_mono(read_media_audio, nullptr);
    if (sAudioStream == GBA_MOD_AUDIO_STREAM_INVALID ||
        !gba_mod_audio_stream_set_enabled(sAudioStream, 1) ||
        !gba_mod_audio_stream_play(sAudioStream, 100)) {
        if (sAudioStream != GBA_MOD_AUDIO_STREAM_INVALID) {
            gba_mod_audio_stream_stop(sAudioStream);
            gba_mod_audio_stream_set_enabled(sAudioStream, 0);
        }
        sAudioStream = GBA_MOD_AUDIO_STREAM_INVALID;
        sStreamPlaying = false;
        sEnabled = false;
        sFallbackReason = "audio_stream_unavailable";
        return;
    }
    sStreamPlaying = true;
    sEnabled = true;
    sFallbackReason = "guest_scene_inactive";
    sLastMediaId = 0xFFFF;
    sLastInvocation = 0;
    sLastElapsedVblanks = 0;
    sLastStateEpoch = ~std::uint64_t{0};
}

bool read_scene_state(GbaHostSceneState& out) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || kGbaHostSceneStateAddress < 0x03000000u ||
        kGbaHostSceneStateAddress + kGbaHostSceneStateSize > 0x03008000u ||
        kGbaHostSceneStateIwramOffset + sizeof(out) > 0x8000u ||
        kGbaHostSceneStateSize != sizeof(out))
        return false;
    const volatile auto* state = reinterpret_cast<const volatile GbaHostSceneState*>(
        bus->iwram_ptr() + kGbaHostSceneStateIwramOffset);
    const std::uint16_t before = state->sequence;
    if (before & 1u) return false;
    out.version = state->version;
    out.size = state->size;
    out.sequence = before;
    out.flags = state->flags;
    out.invocation_id = state->invocation_id;
    out.scene_id = state->scene_id;
    out.media_id = state->media_id;
    out.frame_index = state->frame_index;
    out.player_state = state->player_state;
    out.elapsed_vblanks = state->elapsed_vblanks;
    out.total_vblanks = state->total_vblanks;
    out.loop_count = state->loop_count;
    return before == state->sequence;
}

void observe_scene(const gbarecomp::ExtendedViewFrameInfo* frame) {
    if (!frame) {
        std::lock_guard<std::mutex> status_lock(sStatusMutex);
        sFramePublished = false;
        return;
    }
    std::lock_guard<std::mutex> status_lock(sStatusMutex);
    GbaHostSceneState state{};
    if (!read_scene_state(state) || state.version != 2 || state.size != sizeof(state) ||
        state.invocation_id == 0 || (state.flags & ~kHostStateKnownFlags) != 0) {
        sGuestStateValid = false;
        sGuestSceneActive = false;
        sFramePublished = false;
        sFallbackReason = "guest_state_invalid";
        if (sEnabled) {
            stop_stream();
            gba_mod_clear_foreign_background();
        }
        sLastMediaId = 0xFFFF;
        return;
    }
    sGuestStateValid = true;
    sLastObservedState = state;
    const bool active = (state.flags & kHostStateActive) != 0;
    sGuestSceneActive = active && (state.flags & kHostStateFrameVisible) != 0;
    if (!active || !(state.flags & kHostStateFrameVisible)) {
        sFramePublished = false;
        sFallbackReason = "guest_scene_inactive";
        if (sEnabled) {
            stop_stream();
            gba_mod_clear_foreign_background();
        }
        sLastMediaId = 0xFFFF;
        return;
    }
    if (!sEnabled || !sPackageLoaded) {
        sFramePublished = false;
        if (!sActivationAttempted)
            sFallbackReason = "plugin_not_selected";
        return;
    }
    const bool guest_music_enabled =
        (state.flags & kHostStateGuestMusicEnabled) != 0;
    const bool guest_looping = (state.flags & kHostStateLooping) != 0;
    if (state.scene_id != sMedia.scene_id() ||
        state.total_vblanks != sMedia.total_vblanks() ||
        guest_looping != sMedia.loops() || state.frame_index >= sMedia.frame_count() ||
        state.media_id != sMedia.media_id(state.frame_index) ||
        sMedia.frame_for_elapsed(state.elapsed_vblanks) != state.frame_index ||
        state.player_state > 5 ||
        state.elapsed_vblanks >= (static_cast<std::uint64_t>(state.loop_count) + 1) *
                                     state.total_vblanks) {
        sFramePublished = false;
        sFallbackReason = "package_scene_mismatch";
        stop_stream();
        gba_mod_clear_foreign_background();
        sLastMediaId = 0xFFFF;
        return;
    }
    const bool rewound = state.elapsed_vblanks < sLastElapsedVblanks;
    const bool restored = frame->state_epoch != sLastStateEpoch || rewound;
    const std::size_t audio_cursor =
        sMedia.sample_for_elapsed(state.elapsed_vblanks);
    sAudioCursor.store(audio_cursor, std::memory_order_relaxed);
    sGuestAudioCursor.store(audio_cursor, std::memory_order_relaxed);
    sAudioMuted.store(!guest_music_enabled, std::memory_order_relaxed);
    if (sAudioStream != GBA_MOD_AUDIO_STREAM_INVALID &&
        !gba_mod_audio_stream_set_native_gain_percent(
            sAudioStream, guest_music_enabled ? 0 : 100)) {
        stop_stream();
        sFramePublished = false;
        sFallbackReason = "audio_stream_error";
        gba_mod_clear_foreign_background();
        sLastMediaId = 0xFFFF;
        return;
    }
    if (restored || state.invocation_id != sLastInvocation ||
        state.media_id != sLastMediaId) {
        const std::uint16_t* pixels = sMedia.frame(state.frame_index);
        if (!pixels || !gba_mod_publish_foreign_background(kPluginId, pixels)) {
            sFramePublished = false;
            sFallbackReason = "frame_publish_failed";
            sLastMediaId = 0xFFFF;
            return;
        }
        ++sPublishedFrameCount;
        sLastStateEpoch = frame->state_epoch;
        sLastInvocation = state.invocation_id;
        sLastMediaId = state.media_id;
        sLastPublishedEpoch = frame->state_epoch;
    }
    sFramePublished = sLastPublishedEpoch == frame->state_epoch &&
                      sLastInvocation == state.invocation_id &&
                      sLastMediaId == state.media_id;
    sFallbackReason = sFramePublished ? "host_media_active" : "frame_not_published";
    sLastElapsedVblanks = state.elapsed_vblanks;
    // Savestate restore stops stream delivery but retains this capability.
    // The state epoch changes, so start the bounded source again for the
    // restored active scene. Also start it after an ordinary inactive period.
    if (!sStreamPlaying || restored) {
        if (gba_mod_audio_stream_play(sAudioStream, 100))
            sStreamPlaying = true;
    }
}
}  // namespace

int scene_media_tcp_command(
    const char* request,
    void (*write)(void* ctx, const char* data, std::size_t len),
    void* write_ctx) {
    if (!request || !write ||
        std::string_view(request).find("\"gallery_media_status\"") ==
            std::string_view::npos)
        return 0;
    std::lock_guard<std::mutex> status_lock(sStatusMutex);
    const char* backend = sEnabled && sPackageLoaded && sGuestStateValid &&
                          sGuestSceneActive && sFramePublished
                              ? "host_media" : "gba_fallback";
    const GbaHostSceneState& state = sLastObservedState;
    char response[768];
    const int length = std::snprintf(
        response, sizeof(response),
        "{\"ok\":true,\"backend\":\"%s\",\"fallback_reason\":\"%s\","
        "\"package_loaded\":%s,\"guest_state_valid\":%s,\"guest_scene_active\":%s,"
        "\"frame_published\":%s,\"audio_stream_active\":%s,\"audio_muted\":%s,"
        "\"scene_id\":%u,\"media_id\":%u,\"frame_index\":%u,"
        "\"player_state\":%u,\"elapsed_vblanks\":%u,\"total_vblanks\":%u,"
        "\"loop_count\":%u,\"invocation_id\":%u,\"flags\":%u,"
        "\"state_epoch\":%llu,\"audio_cursor_samples\":%zu,"
        "\"published_frames\":%u}",
        backend, sFallbackReason,
        sPackageLoaded ? "true" : "false",
        sGuestStateValid ? "true" : "false",
        sGuestSceneActive ? "true" : "false",
        sFramePublished ? "true" : "false",
        sStreamPlaying ? "true" : "false",
        sAudioMuted.load(std::memory_order_relaxed) ? "true" : "false",
        state.scene_id, state.media_id, state.frame_index, state.player_state,
        state.elapsed_vblanks, state.total_vblanks, state.loop_count,
        state.invocation_id, state.flags,
        static_cast<unsigned long long>(sLastPublishedEpoch),
        sGuestAudioCursor.load(std::memory_order_relaxed),
        sPublishedFrameCount);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(response))
        return 0;
    write(write_ctx, response, static_cast<std::size_t>(length));
    return 1;
}

void configure_scene_media(gbarecomp::RunOptions& options) {
    options.extended_view_frame = observe_scene;
    options.tcp_command = scene_media_tcp_command;
}

GBA_MOD_CONSTRUCTOR(register_scene_media_observer) {
    gba_mod_register_reset_callback(reset_media);
    gba_mod_register_activation_plugin(kPluginId, activate_media);
}
}  // namespace emerald_media
