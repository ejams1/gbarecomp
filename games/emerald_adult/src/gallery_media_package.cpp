#include "gallery_media_package.h"

#include "mini_json.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

namespace emerald_media {
namespace {
constexpr std::size_t kMaxManifestBytes = 256u * 1024u;
constexpr std::size_t kMaxSceneBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxFrames = 12;
constexpr std::size_t kMaxChunks = 14;
constexpr std::uint32_t kFrameBytes = 240u * 160u * 2u;
constexpr std::uint32_t kAudioRate = 65536;
constexpr std::uint32_t kGuestClock = 60;

void fail(std::string* error, const char* message) {
    if (error) *error = message;
}

bool exact_u32(const gbarecomp::json::Value* value, std::uint32_t& out) {
    if (!value || !value->is_number() || !std::isfinite(value->number) ||
        value->number < 0 || value->number > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(value->number) != value->number)
        return false;
    out = static_cast<std::uint32_t>(value->number);
    return true;
}

bool hex_digest(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string asset_id_for_filename(const std::string& filename) {
    std::string id = "media-";
    for (unsigned char c : filename) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            id.push_back(static_cast<char>(c));
        } else if (c >= 'A' && c <= 'Z') {
            id.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (id.back() != '-') {
            id.push_back('-');
        }
    }
    while (id.size() > 7 && id.back() == '-') id.pop_back();
    return id;
}

bool read_bounded(const char* path, std::size_t expected,
                  std::vector<std::uint8_t>& bytes) {
    if (!path || !*path || expected > kMaxSceneBytes) return false;
    std::error_code ec;
    const auto actual_size = std::filesystem::file_size(path, ec);
    if (ec || actual_size != expected || actual_size > kMaxSceneBytes) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.resize(expected);
    if (expected != 0)
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(expected));
    return file.good() || (file.eof() && static_cast<std::size_t>(file.gcount()) == expected);
}

bool read_asset(AssetResolver resolver, void* context, const std::string& filename,
                std::size_t expected, std::vector<std::uint8_t>& bytes) {
    // The path comes only from the runtime's committed, hash-checked package
    // asset resolver. The untrusted manifest controls only a validated ID.
    const std::string id = asset_id_for_filename(filename);
    const char* path = resolver ? resolver(context, id.c_str()) : nullptr;
    return read_bounded(path, expected, bytes);
}

bool read_audio_asset(AssetResolver resolver, void* context,
                      const std::string& filename, std::size_t expected_bytes,
                      std::vector<std::int16_t>& samples) {
    const std::string id = asset_id_for_filename(filename);
    const char* path = resolver ? resolver(context, id.c_str()) : nullptr;
    if (!path || !*path || expected_bytes > kMaxSceneBytes || expected_bytes % 2)
        return false;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != expected_bytes || ec) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    samples.resize(expected_bytes / 2);
    file.read(reinterpret_cast<char*>(samples.data()),
              static_cast<std::streamsize>(expected_bytes));
    if (!file.good() && !(file.eof() &&
        static_cast<std::size_t>(file.gcount()) == expected_bytes)) return false;
    if constexpr (std::endian::native == std::endian::big) {
        auto* raw = reinterpret_cast<std::uint8_t*>(samples.data());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const std::uint16_t value = static_cast<std::uint16_t>(raw[i * 2]) |
                static_cast<std::uint16_t>(raw[i * 2 + 1] << 8);
            samples[i] = static_cast<std::int16_t>(value);
        }
    }
    return true;
}

const gbarecomp::json::Value* field(const gbarecomp::json::Value* value,
                                    const char* key,
                                    gbarecomp::json::Value::Type type) {
    const auto* result = value ? value->get(key) : nullptr;
    return result && result->type == type ? result : nullptr;
}

}  // namespace

void GalleryMediaPackage::clear() {
    frames_.clear();
    timeline_.clear();
    audio_.clear();
    total_vblanks_ = 0;
    total_samples_ = 0;
    scene_id_ = 0;
    media_id_base_ = 0;
    loops_ = false;
}

bool GalleryMediaPackage::load(const char* manifest_path,
                               AssetResolver resolver,
                               void* resolver_context,
                               std::string* error) {
    clear();
    std::vector<std::uint8_t> manifest_bytes;
    std::error_code ec;
    const auto manifest_size = manifest_path
        ? std::filesystem::file_size(manifest_path, ec) : 0;
    if (!manifest_path || ec || manifest_size == 0 ||
        manifest_size > kMaxManifestBytes ||
        !read_bounded(manifest_path, static_cast<std::size_t>(manifest_size), manifest_bytes)) {
        fail(error, "gallery manifest is missing or exceeds its size bound");
        return false;
    }
    gbarecomp::json::Value root;
    std::string json_error;
    if (!gbarecomp::json::parse(
            std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()),
                             manifest_bytes.size()), root, &json_error) ||
        !root.is_object()) {
        fail(error, "gallery manifest is not valid JSON");
        return false;
    }
    std::uint32_t schema = 0;
    const std::string scene_name = root.str("scene_id");
    std::uint16_t scene_id = 0, media_id_base = 0;
    std::string media_name;
    if (scene_name == "SCENE_ID_NEUTRAL_MEADOW") {
        scene_id = 0x4000; media_id_base = 0x4100; media_name = "MEADOW";
    } else if (scene_name == "SCENE_ID_NEUTRAL_COAST") {
        scene_id = 0x4001; media_id_base = 0x4110; media_name = "COAST";
    } else if (scene_name == "SCENE_ID_NEUTRAL_NIGHT") {
        scene_id = 0x4002; media_id_base = 0x4120; media_name = "NIGHT";
    }
    if (!exact_u32(root.get("schema_version"), schema) || schema != 1 || !scene_id) {
        fail(error, "unsupported gallery media schema or scene ID");
        return false;
    }
    const auto* source = field(&root, "source", gbarecomp::json::Value::Type::Object);
    const std::string profile = source ? source->str("frame_size_profile") : "";
    std::uint32_t source_width = 0, source_height = 0;
    if (!exact_u32(source ? source->get("source_width") : nullptr, source_width) ||
        !exact_u32(source ? source->get("source_height") : nullptr, source_height) ||
        !((profile == "native_3_2" && source_width == 240 && source_height == 160) ||
          (profile == "landscape_16_9" && source_width == 160 && source_height == 90) ||
          (profile == "portrait_9_16" && source_width == 90 && source_height == 160))) {
        fail(error, "unsupported or inconsistent source video dimensions");
        return false;
    }
    const auto* frame_info = field(&root, "frames", gbarecomp::json::Value::Type::Object);
    std::uint32_t width = 0, height = 0, frame_bytes = 0;
    const auto* chunks = field(frame_info, "chunks", gbarecomp::json::Value::Type::Array);
    if (!frame_info || frame_info->str("format") != "BGR555LE" ||
        !exact_u32(frame_info->get("width"), width) || width != 240 ||
        !exact_u32(frame_info->get("height"), height) || height != 160 ||
        !exact_u32(frame_info->get("frame_bytes"), frame_bytes) ||
        frame_bytes != kFrameBytes || !chunks || chunks->array.empty() ||
        chunks->array.size() > kMaxChunks) {
        fail(error, "gallery frames must be bounded 240x160 BGR555LE chunks");
        return false;
    }
    const auto* audio_info = field(&root, "audio", gbarecomp::json::Value::Type::Object);
    std::uint32_t sample_rate = 0, channels = 0, sample_count = 0;
    std::uint32_t audio_bytes_count = 0;
    if (!audio_info || audio_info->str("format") !=
            "signed 16-bit little-endian mono PCM" ||
        !exact_u32(audio_info->get("sample_rate"), sample_rate) ||
        sample_rate != kAudioRate ||
        !exact_u32(audio_info->get("channels"), channels) || channels != 1 ||
        !exact_u32(audio_info->get("sample_count"), sample_count) ||
        !exact_u32(audio_info->get("expanded_bytes"), audio_bytes_count) ||
        sample_count == 0 || sample_count > kMaxSceneBytes / 2 ||
        audio_bytes_count != sample_count * 2u ||
        !hex_digest(audio_info->str("sha256"))) {
        fail(error, "gallery audio must be bounded mono S16LE at 65536 Hz");
        return false;
    }
    const auto* timeline_info = field(&root, "timeline", gbarecomp::json::Value::Type::Object);
    const auto* timeline_frames = field(timeline_info, "frames", gbarecomp::json::Value::Type::Array);
    std::uint32_t guest_clock = 0;
    if (!timeline_info || !timeline_frames || timeline_frames->array.empty() ||
        timeline_frames->array.size() > kMaxFrames ||
        !exact_u32(timeline_info->get("guest_clock_hz"), guest_clock) ||
        guest_clock != kGuestClock || timeline_info->integer("sample_rate", 0) != kAudioRate ||
        (timeline_info->str("loop_policy") != "REPEAT" &&
         timeline_info->str("loop_policy") != "ONCE")) {
        fail(error, "gallery guest timeline is unsupported or incomplete");
        return false;
    }
    std::uint32_t total_vblanks = 0, total_samples = 0;
    if (!exact_u32(timeline_info->get("total_vblanks"), total_vblanks) ||
        !exact_u32(timeline_info->get("total_samples"), total_samples) ||
        total_vblanks == 0 || total_vblanks > 3600 || total_samples != sample_count ||
        total_samples != (static_cast<std::uint64_t>(total_vblanks) * kAudioRate + 30) / 60) {
        fail(error, "gallery total duration does not match its PCM sample count");
        return false;
    }
    const auto* cost = field(&root, "cost", gbarecomp::json::Value::Type::Object);
    std::uint32_t expanded_total = 0, media_files = 0;
    if (!cost || !exact_u32(cost->get("host_expanded_bytes"), expanded_total) ||
        expanded_total > kMaxSceneBytes ||
        !exact_u32(cost->get("host_media_file_count"), media_files) ||
        media_files > kMaxChunks + 1) {
        fail(error, "gallery media exceeds its expanded size or file-count bounds");
        return false;
    }

    std::vector<std::vector<std::uint16_t>> frames;
    std::vector<TimelineFrame> timeline;
    std::uint32_t first_frame = 0;
    std::uint32_t loaded_bytes = 0;
    for (const auto& item : timeline_frames->array) {
        std::uint32_t index = 0, start_vblank = 0, end_vblank = 0;
        std::uint32_t start_sample = 0, end_sample = 0, duration = 0;
        if (!exact_u32(item.get("index"), index) || index != timeline.size() ||
            !exact_u32(item.get("start_vblank"), start_vblank) ||
            !exact_u32(item.get("end_vblank"), end_vblank) ||
            !exact_u32(item.get("duration_vblanks"), duration) || duration == 0 ||
            end_vblank != start_vblank + duration ||
            !exact_u32(item.get("start_sample"), start_sample) ||
            !exact_u32(item.get("end_sample"), end_sample) ||
            end_sample <= start_sample ||
            start_sample != (static_cast<std::uint64_t>(start_vblank) * kAudioRate + 30) / 60 ||
            end_sample != (static_cast<std::uint64_t>(end_vblank) * kAudioRate + 30) / 60 ||
            item.str("media_id") != "SCENE_MEDIA_ID_" + media_name + "_" + std::to_string(index) ||
            start_vblank != (timeline.empty() ? 0 : timeline.back().end_vblank) ||
            start_sample != (timeline.empty() ? 0 : timeline.back().end_sample)) {
            fail(error, "gallery frame timeline has invalid IDs or boundaries");
            return false;
        }
        timeline.push_back({start_vblank, end_vblank, start_sample, end_sample,
                            item.str("media_id")});
    }
    if (timeline.size() != timeline_frames->array.size() ||
        timeline.back().end_vblank != total_vblanks ||
        timeline.back().end_sample != total_samples ||
        (media_name == "COAST" && timeline.size() > 3) ||
        (media_name != "COAST" && timeline.size() > 4)) {
        fail(error, "gallery frame timeline does not cover the full scene");
        return false;
    }
    for (const auto& chunk : chunks->array) {
        const std::string filename = chunk.str("file");
        std::uint32_t chunk_first = 0, chunk_frames = 0, chunk_size = 0;
        if (!exact_u32(chunk.get("first_frame"), chunk_first) ||
            !exact_u32(chunk.get("frame_count"), chunk_frames) ||
            !exact_u32(chunk.get("expanded_bytes"), chunk_size) ||
            !hex_digest(chunk.str("sha256")) || filename.empty() ||
            filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos ||
            chunk_first != first_frame || chunk_frames == 0 || chunk_frames > kMaxFrames ||
            chunk_size != chunk_frames * kFrameBytes ||
            first_frame + chunk_frames > timeline.size()) {
            fail(error, "gallery frame chunk declaration is invalid");
            return false;
        }
        std::vector<std::uint8_t> bytes;
        if (!read_asset(resolver, resolver_context, filename, chunk_size, bytes)) {
            fail(error, "gallery frame chunk is missing, stale, or has a wrong size/hash");
            return false;
        }
        loaded_bytes += chunk_size;
        for (std::uint32_t local = 0; local < chunk_frames; ++local) {
            std::vector<std::uint16_t> frame(kPixels);
            const std::size_t byte_at = static_cast<std::size_t>(local) * kFrameBytes;
            for (std::size_t pixel = 0; pixel < kPixels; ++pixel) {
                const std::uint16_t value = static_cast<std::uint16_t>(bytes[byte_at + pixel * 2]) |
                    static_cast<std::uint16_t>(bytes[byte_at + pixel * 2 + 1] << 8);
                if (value & 0x8000u) {
                    fail(error, "gallery BGR555LE frame uses the reserved high bit");
                    return false;
                }
                frame[pixel] = value;
            }
            frames.push_back(std::move(frame));
        }
        first_frame += chunk_frames;
    }
    if (first_frame != timeline.size()) {
        fail(error, "gallery frame chunks do not cover all timeline frames");
        return false;
    }
    const std::string audio_filename = audio_info->str("file");
    if (audio_filename.empty() || audio_filename.find('/') != std::string::npos ||
        audio_filename.find('\\') != std::string::npos) {
        fail(error, "gallery audio filename is invalid");
        return false;
    }
    std::vector<std::int16_t> audio;
    if (!read_audio_asset(resolver, resolver_context, audio_filename,
                          audio_bytes_count, audio)) {
        fail(error, "gallery PCM audio is missing, stale, or has a wrong size/hash");
        return false;
    }
    loaded_bytes += audio_bytes_count;
    if (loaded_bytes != expanded_total || loaded_bytes > kMaxSceneBytes) {
        fail(error, "gallery actual media size disagrees with the manifest");
        return false;
    }
    frames_ = std::move(frames);
    timeline_ = std::move(timeline);
    audio_ = std::move(audio);
    total_vblanks_ = total_vblanks;
    total_samples_ = total_samples;
    scene_id_ = scene_id;
    media_id_base_ = media_id_base;
    loops_ = timeline_info->str("loop_policy") == "REPEAT";
    return true;
}

const std::uint16_t* GalleryMediaPackage::frame(std::size_t index) const {
    return index < frames_.size() ? frames_[index].data() : nullptr;
}

std::uint16_t GalleryMediaPackage::media_id(std::size_t frame_index) const {
    return frame_index < frames_.size()
        ? static_cast<std::uint16_t>(media_id_base_ + frame_index) : 0xFFFF;
}

std::size_t GalleryMediaPackage::frame_for_elapsed(std::uint64_t elapsed) const {
    if (timeline_.empty() || total_vblanks_ == 0) return frames_.size();
    std::uint64_t tick = loops_ ? elapsed % total_vblanks_
                                : std::min<std::uint64_t>(elapsed, total_vblanks_ - 1);
    for (std::size_t i = 0; i < timeline_.size(); ++i)
        if (tick >= timeline_[i].start_vblank && tick < timeline_[i].end_vblank)
            return i;
    return frames_.size();
}

std::size_t GalleryMediaPackage::sample_for_elapsed(std::uint64_t elapsed) const {
    if (total_samples_ == 0 || total_vblanks_ == 0) return 0;
    const std::uint64_t tick = loops_ ? elapsed % total_vblanks_
                                      : std::min<std::uint64_t>(elapsed, total_vblanks_);
    const std::uint64_t sample = (tick * kAudioRate + kGuestClock / 2) / kGuestClock;
    const std::uint64_t bounded = std::min<std::uint64_t>(sample, total_samples_);
    return static_cast<std::size_t>(bounded);
}

std::size_t GalleryMediaPackage::read_audio(std::size_t offset,
                                           std::int16_t* dst,
                                           std::size_t count,
                                           bool loop) const {
    if (!dst || audio_.empty() || count == 0) return 0;
    if (loop) {
        for (std::size_t i = 0; i < count; ++i)
            dst[i] = audio_[(offset + i) % audio_.size()];
        return count;
    }
    if (offset >= audio_.size()) return 0;
    const std::size_t produced = std::min(count, audio_.size() - offset);
    std::copy_n(audio_.data() + offset, produced, dst);
    return produced;
}

}  // namespace emerald_media
