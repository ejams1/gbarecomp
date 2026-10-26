#include "gallery_media_package.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::map<std::string, std::string> g_assets;

const char* resolve(void*, const char* id) {
    const auto it = g_assets.find(id ? id : "");
    return it == g_assets.end() ? nullptr : it->second.c_str();
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "gallery-media-package-tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return 1;
    std::vector<std::uint8_t> frames(2u * 76800u);
    for (std::size_t pixel = 0; pixel < 240u * 160u; ++pixel) {
        frames[pixel * 2] = 0x1F;
        frames[pixel * 2 + 1] = 0;
        const std::size_t second = 76800u + pixel * 2;
        frames[second] = 0xE0;
        frames[second + 1] = 0x03;
    }
    std::vector<std::uint8_t> audio(2185u * 2u);
    for (std::size_t i = 0; i < audio.size() / 2; ++i) {
        const std::int16_t value = static_cast<std::int16_t>(i % 1000);
        audio[i * 2] = static_cast<std::uint8_t>(value);
        audio[i * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    const fs::path frames_path = dir / "frames_000.bgr555le";
    const fs::path audio_path = dir / "audio.s16le";
    const fs::path manifest_path = dir / "manifest.json";
    if (!write_file(frames_path, frames) || !write_file(audio_path, audio)) return 1;
    g_assets["media-frames-000-bgr555le"] = frames_path.string();
    g_assets["media-audio-s16le"] = audio_path.string();
    const std::string digest(64, '0');
    const std::string manifest =
        "{\"schema_version\":1,\"scene_id\":\"SCENE_ID_NEUTRAL_COAST\","
        "\"source\":{\"source_width\":240,\"source_height\":160,"
        "\"frame_size_profile\":\"native_3_2\"},"
        "\"frames\":{\"width\":240,\"height\":160,\"format\":\"BGR555LE\","
        "\"frame_bytes\":76800,\"chunks\":[{\"file\":\"frames_000.bgr555le\","
        "\"first_frame\":0,\"frame_count\":2,\"expanded_bytes\":153600,\"sha256\":\"" + digest + "\"}]},"
        "\"audio\":{\"format\":\"signed 16-bit little-endian mono PCM\","
        "\"sample_rate\":65536,\"channels\":1,\"sample_count\":2185,"
        "\"file\":\"audio.s16le\",\"expanded_bytes\":4370,\"sha256\":\"" + digest + "\"},"
        "\"timeline\":{\"guest_clock_hz\":60,\"loop_policy\":\"REPEAT\","
        "\"sample_rate\":65536,\"total_vblanks\":2,\"total_samples\":2185,"
        "\"frames\":["
        "{\"index\":0,\"media_id\":\"SCENE_MEDIA_ID_COAST_0\",\"duration_vblanks\":1,"
        "\"start_vblank\":0,\"end_vblank\":1,\"start_sample\":0,\"end_sample\":1092},"
        "{\"index\":1,\"media_id\":\"SCENE_MEDIA_ID_COAST_1\",\"duration_vblanks\":1,"
        "\"start_vblank\":1,\"end_vblank\":2,\"start_sample\":1092,\"end_sample\":2185}]},"
        "\"cost\":{\"host_expanded_bytes\":157970,\"host_media_file_count\":2}}";
    if (!write_text(manifest_path, manifest)) return 1;

    emerald_media::GalleryMediaPackage package;
    std::string error;
    if (!package.load(manifest_path.string().c_str(), resolve, nullptr, &error) ||
        package.scene_id() != 0x4001 || package.frame_count() != 2 ||
        package.total_vblanks() != 2 || package.total_samples() != 2185 ||
        !package.loops() || package.media_id(1) != 0x4111 ||
        package.frame_for_elapsed(0) != 0 || package.frame_for_elapsed(1) != 1 ||
        package.frame_for_elapsed(2) != 0 || package.sample_for_elapsed(1) != 1092 ||
        package.sample_for_elapsed(2) != 0 || package.sample_for_elapsed(4) != 0 ||
        package.sample_for_elapsed(5) != 1092 || package.sample_for_elapsed(6) != 0 ||
        !package.frame(0) ||
        package.frame(0)[0] != 0x001F || package.frame(1)[0] != 0x03E0) {
        std::cerr << "valid gallery video package rejected or timeline decoded incorrectly: "
                  << error << '\n';
        fs::remove_all(dir, ec);
        return 1;
    }
    std::int16_t mixed[4]{};
    if (package.read_audio(2183, mixed, 4, true) != 4 ||
        mixed[0] != 183 || mixed[1] != 184 || mixed[2] != 0 || mixed[3] != 1) {
        std::cerr << "looped S16 audio read did not wrap at the exact sample boundary\n";
        fs::remove_all(dir, ec);
        return 1;
    }
    std::string alternate = manifest;
    const auto profile_at = alternate.find("native_3_2");
    alternate.replace(profile_at, std::string("native_3_2").size(), "portrait_9_16");
    const auto width_at = alternate.find("\"source_width\":240");
    alternate.replace(width_at, std::string("\"source_width\":240").size(),
                      "\"source_width\":90");
    const auto height_at = alternate.find("\"source_height\":160");
    alternate.replace(height_at, std::string("\"source_height\":160").size(),
                      "\"source_height\":160");
    auto replace_all = [&](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = alternate.find(from, pos)) != std::string::npos) {
            alternate.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("SCENE_ID_NEUTRAL_COAST", "SCENE_ID_NEUTRAL_NIGHT");
    replace_all("SCENE_MEDIA_ID_COAST_", "SCENE_MEDIA_ID_NIGHT_");
    write_text(manifest_path, alternate);
    if (!package.load(manifest_path.string().c_str(), resolve, nullptr, &error) ||
        package.scene_id() != 0x4002 || package.media_id(1) != 0x4121) {
        std::cerr << "90x160 portrait source video profile was rejected: " << error << '\n';
        fs::remove_all(dir, ec);
        return 1;
    }
    alternate = manifest;
    const auto landscape_profile = alternate.find("native_3_2");
    alternate.replace(landscape_profile, std::string("native_3_2").size(), "landscape_16_9");
    const auto landscape_width = alternate.find("\"source_width\":240");
    alternate.replace(landscape_width, std::string("\"source_width\":240").size(),
                      "\"source_width\":160");
    const auto landscape_height = alternate.find("\"source_height\":160");
    alternate.replace(landscape_height, std::string("\"source_height\":160").size(),
                      "\"source_height\":90");
    replace_all("SCENE_ID_NEUTRAL_COAST", "SCENE_ID_NEUTRAL_MEADOW");
    replace_all("SCENE_MEDIA_ID_COAST_", "SCENE_MEDIA_ID_MEADOW_");
    write_text(manifest_path, alternate);
    if (!package.load(manifest_path.string().c_str(), resolve, nullptr, &error) ||
        package.scene_id() != 0x4000 || package.media_id(1) != 0x4101) {
        std::cerr << "160x90 landscape source video profile was rejected: " << error << '\n';
        fs::remove_all(dir, ec);
        return 1;
    }
    std::string changed = manifest;
    const std::string end_value = "\"end_sample\":1092";
    const auto at = changed.find(end_value);
    changed.replace(at, end_value.size(), "\"end_sample\":1091");
    write_text(manifest_path, changed);
    if (package.load(manifest_path.string().c_str(), resolve, nullptr, &error)) {
        std::cerr << "inconsistent audio timeline was accepted\n";
        fs::remove_all(dir, ec);
        return 1;
    }
    fs::remove_all(dir, ec);
    std::cout << "gallery media package: bounded BGR555 frames, guest timeline, and PCM loop passed\n";
    return 0;
}
