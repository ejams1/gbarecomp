#include "scene_media_observer.h"

#include "gba_bus.h"
#include "mod_audio.h"
#include "mod_runtime.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "../generated/host_scene_state_address.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace emerald_media {
namespace {
constexpr char kPluginId[] = "pokemon-adult.gallery-media-seam.v1";
constexpr std::uint16_t kSceneIdCoast = 0x4001;
constexpr std::uint16_t kMediaIdCoast0 = 0x4110;
constexpr std::uint16_t kHostStateActive = 1u << 0;
constexpr std::uint16_t kHostStateFrameVisible = 1u << 1;
constexpr std::uint16_t kHostStateLooping = 1u << 2;
constexpr std::uint16_t kHostStateKnownFlags =
    kHostStateActive | kHostStateFrameVisible | kHostStateLooping;
constexpr std::uint32_t kCoastDurationVBlanks = 30;
constexpr std::size_t kPixels = 240u * 160u;

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

constexpr std::uint16_t color(std::uint16_t r, std::uint16_t g, std::uint16_t b) {
    return static_cast<std::uint16_t>((r & 31u) | ((g & 31u) << 5) |
                                      ((b & 31u) << 10));
}

using Frame = std::array<std::uint16_t, kPixels>;
using Frames = std::array<Frame, 3>;

Frames make_neutral_coast_frames() {
    Frames frames{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        Frame& frame = frames[index];
        for (std::uint16_t y = 0; y < 160; ++y) {
            for (std::uint16_t x = 0; x < 240; ++x) {
                std::uint16_t r, g, b;
                if (y < 89) {
                    const std::uint16_t shade = static_cast<std::uint16_t>((y / 18 + index) % 5);
                    r = 7 + shade; g = 15 + shade; b = 24 + shade;
                } else if (y < 119) {
                    const std::uint16_t ripple = static_cast<std::uint16_t>(((x / 24 + y / 5 + index) % 4));
                    r = 4 + ripple; g = 12 + ripple; b = 18 + ripple;
                } else {
                    const int coast = 116 + static_cast<int>((x + index * 9) / 12 % 11);
                    if (static_cast<int>(y) >= coast) {
                        r = 17; g = 19; b = 13;
                    } else {
                        r = 5; g = 12; b = 18;
                    }
                }
                const int dx = static_cast<int>(x) - 191;
                const int dy = static_cast<int>(y) - 39;
                if (dx * dx + dy * dy <= 8 * 8) {
                    r = 30; g = 26; b = 16;
                }
                frame[static_cast<std::size_t>(y) * 240 + x] = color(r, g, b);
            }
        }
    }
    return frames;
}

// Static immutable host media. Initialization precedes the emulation loop;
// the per-frame callback neither allocates nor decodes.
const Frames kNeutralCoastFrames = make_neutral_coast_frames();
bool sEnabled;
std::uint16_t sLastMediaId = 0xFFFF;
std::uint32_t sLastInvocation = 0;
std::uint64_t sLastStateEpoch = ~std::uint64_t{0};
GBAModAudioStream sAudioStream = GBA_MOD_AUDIO_STREAM_INVALID;
bool sStreamPlaying = false;

// The initial trusted shell exercises the bounded pull-stream lifecycle while
// contributing no sound over the guest's existing music and effects.
std::size_t read_silence(void*, std::int16_t* dst, std::size_t max_frames) {
    if (!dst || max_frames > GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES) return 0;
    for (std::size_t i = 0; i < max_frames; ++i) dst[i] = 0;
    return max_frames;
}

void stop_stream() {
    if (sAudioStream != GBA_MOD_AUDIO_STREAM_INVALID && sStreamPlaying)
        gba_mod_audio_stream_stop(sAudioStream);
    sStreamPlaying = false;
}

void reset_media() {
    // The runtime retires stream capabilities before reset callbacks.
    sAudioStream = GBA_MOD_AUDIO_STREAM_INVALID;
    sStreamPlaying = false;
    sEnabled = false;
    sLastMediaId = 0xFFFF;
    sLastInvocation = 0;
    sLastStateEpoch = ~std::uint64_t{0};
    gba_mod_clear_foreign_background();
}

void activate_media() {
    sAudioStream = gba_mod_audio_stream_register_s16_mono(read_silence, nullptr);
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
        return;
    }
    sStreamPlaying = true;
    sEnabled = true;
    sLastMediaId = 0xFFFF;
    sLastInvocation = 0;
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
    if (!sEnabled || !frame) return;
    GbaHostSceneState state{};
    if (!read_scene_state(state) || state.version != 1 || state.size != sizeof(state) ||
        state.invocation_id == 0 || (state.flags & ~kHostStateKnownFlags) != 0) {
        stop_stream();
        gba_mod_clear_foreign_background();
        sLastMediaId = 0xFFFF;
        return;
    }
    const bool active = (state.flags & kHostStateActive) != 0;
    if (!active || !(state.flags & kHostStateFrameVisible)) {
        stop_stream();
        gba_mod_clear_foreign_background();
        sLastMediaId = 0xFFFF;
        return;
    }
    if (state.scene_id != kSceneIdCoast || state.total_vblanks != kCoastDurationVBlanks ||
        !(state.flags & kHostStateLooping) || state.frame_index >= kNeutralCoastFrames.size() ||
        state.media_id != kMediaIdCoast0 + state.frame_index ||
        state.player_state > 5 ||
        state.elapsed_vblanks >= (static_cast<std::uint64_t>(state.loop_count) + 1) *
                                     state.total_vblanks) {
        stop_stream();
        gba_mod_clear_foreign_background();
        sLastMediaId = 0xFFFF;
        return;
    }
    const bool restored = frame->state_epoch != sLastStateEpoch;
    if (frame->state_epoch != sLastStateEpoch || state.invocation_id != sLastInvocation ||
        state.media_id != sLastMediaId) {
        const auto& pixels = kNeutralCoastFrames[state.frame_index];
        if (!gba_mod_publish_foreign_background(kPluginId, pixels.data())) {
            sLastMediaId = 0xFFFF;
            return;
        }
        sLastStateEpoch = frame->state_epoch;
        sLastInvocation = state.invocation_id;
        sLastMediaId = state.media_id;
    }
    // Savestate restore stops stream delivery but retains this capability.
    // The state epoch changes, so start the bounded source again for the
    // restored active scene. Also start it after an ordinary inactive period.
    if (!sStreamPlaying || restored) {
        if (gba_mod_audio_stream_play(sAudioStream, 100))
            sStreamPlaying = true;
    }
}
}  // namespace

void configure_scene_media(gbarecomp::RunOptions& options) {
    options.extended_view_frame = observe_scene;
}

GBA_MOD_CONSTRUCTOR(register_scene_media_observer) {
    gba_mod_register_reset_callback(reset_media);
    gba_mod_register_activation_plugin(kPluginId, activate_media);
}
}  // namespace emerald_media
