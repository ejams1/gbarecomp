// oracle/main.cpp — mGBA-backed reference emulator.
//
// Embeds libmgba and exposes the `emu_*` / `read_emu_*` TCP command
// set from ../TCP.md. Native gbarecomp speaks the matching `read_*`
// commands; a diff harness uses the separately reported guest frame,
// video-frame callback, and wrapper call observations to sync and report
// the first divergence. Wrapper calls are not VBlank IRQ counts.
//
// This binary is the ONLY place libmgba may be linked. Per
// PRINCIPLES.md, the native build has zero copyleft emulator deps.

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <cerrno>
#include <new>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
   using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define CLOSESOCK ::close
#endif

extern "C" {
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/dma.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
}

namespace {

// mGBA emits BIOS SWI and DMA diagnostics at informational levels, often on
// every frame. Keep bounded agent-run reports useful by retaining errors and
// exposing the full stream only when explicitly requested.
void oracle_log(struct mLogger* logger, int category, enum mLogLevel level,
                const char* format, va_list args) {
    (void)logger;
    (void)category;
    if (std::getenv("GBA_ORACLE_VERBOSE") || level == mLOG_FATAL || level == mLOG_ERROR) {
        std::vfprintf(stderr, format, args);
        std::fputc('\n', stderr);
    }
}

struct mLogger g_oracle_logger = {.log = oracle_log};

// ─────────────────────────────────────────────────────────────────────
// JSON micro-helpers
// ─────────────────────────────────────────────────────────────────────

// The protocol is a flat JSON object with scalar values. Parse and validate
// the complete request before dispatch so command names/arguments cannot be
// accidentally found inside an unrelated string value.

struct JsonField {
    enum class Kind { String, Number, Boolean, Null } kind = Kind::Null;
    std::string value;
};
using JsonObject = std::unordered_map<std::string, JsonField>;

void skip_space(std::string_view input, std::size_t& pos) {
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' ||
           input[pos] == '\r' || input[pos] == '\n')) ++pos;
}

bool parse_json_string(std::string_view input, std::size_t& pos,
                       std::string& value) {
    if (pos >= input.size() || input[pos++] != '"') return false;
    value.clear();
    while (pos < input.size()) {
        char c = input[pos++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') { value.push_back(c); continue; }
        if (pos >= input.size()) return false;
        char escape = input[pos++];
        switch (escape) {
        case '"': case '\\': case '/': value.push_back(escape); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            if (pos + 4 > input.size()) return false;
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
                char h = input[pos++];
                int digit = h >= '0' && h <= '9' ? h - '0' :
                    h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                    h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (digit < 0) return false;
                code = (code << 4) | static_cast<unsigned>(digit);
            }
            if (code > 0x7F) return false;
            value.push_back(static_cast<char>(code));
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool parse_json_object(std::string_view input, JsonObject& fields,
                       std::vector<std::string>* order = nullptr) {
    fields.clear();
    if (order) order->clear();
    std::size_t pos = 0;
    skip_space(input, pos);
    if (pos >= input.size() || input[pos++] != '{') return false;
    skip_space(input, pos);
    if (pos < input.size() && input[pos] == '}') {
        ++pos; skip_space(input, pos); return pos == input.size();
    }
    while (pos < input.size()) {
        std::string key;
        if (!parse_json_string(input, pos, key)) return false;
        skip_space(input, pos);
        if (pos >= input.size() || input[pos++] != ':') return false;
        skip_space(input, pos);
        JsonField field;
        if (pos < input.size() && input[pos] == '"') {
            field.kind = JsonField::Kind::String;
            if (!parse_json_string(input, pos, field.value)) return false;
        } else if (input.substr(pos, 4) == "true") {
            field.kind = JsonField::Kind::Boolean; field.value = "true"; pos += 4;
        } else if (input.substr(pos, 5) == "false") {
            field.kind = JsonField::Kind::Boolean; field.value = "false"; pos += 5;
        } else if (input.substr(pos, 4) == "null") {
            field.kind = JsonField::Kind::Null; field.value = "null"; pos += 4;
        } else {
            std::size_t start = pos;
            if (pos < input.size() && input[pos] == '-') ++pos;
            if (pos >= input.size() || input[pos] < '0' || input[pos] > '9') return false;
            if (input[pos] == '0') ++pos;
            else while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (pos < input.size() && (input[pos] == '.' || input[pos] == 'e' || input[pos] == 'E')) return false;
            field.kind = JsonField::Kind::Number;
            field.value.assign(input.substr(start, pos - start));
        }
        if (fields.find(key) != fields.end()) return false;
        if (order) order->push_back(key);
        fields.emplace(std::move(key), std::move(field));
        skip_space(input, pos);
        if (pos < input.size() && input[pos] == '}') {
            ++pos; skip_space(input, pos); return pos == input.size();
        }
        if (pos >= input.size() || input[pos++] != ',') return false;
        skip_space(input, pos);
    }
    return false;
}

bool get_field(std::string_view req, std::string_view quoted_key,
               JsonField& field) {
    if (quoted_key.size() < 2 || quoted_key.front() != '"' || quoted_key.back() != '"') return false;
    JsonObject fields;
    if (!parse_json_object(req, fields)) return false;
    auto it = fields.find(std::string(quoted_key.substr(1, quoted_key.size() - 2)));
    if (it == fields.end()) return false;
    field = it->second;
    return true;
}

void json_emit_hex(std::string& out, const uint8_t* data, std::size_t n) {
    out.reserve(out.size() + n * 2 + 2);
    out.push_back('"');
    static const char* H = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(H[data[i] >> 4]);
        out.push_back(H[data[i] & 0xF]);
    }
    out.push_back('"');
}

bool extract_uint(std::string_view req, std::string_view key, uint64_t& out) {
    JsonField field;
    if (!get_field(req, key, field) ||
        (field.kind != JsonField::Kind::Number && field.kind != JsonField::Kind::String)) return false;
    std::string_view value = field.value;
    if (value.empty() || value.front() == '-') return false;
    int base = 10;
    std::size_t pos = 0;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        pos += 2;
    }
    uint64_t v = 0;
    bool any = false;
    while (pos < value.size()) {
        char c = value[pos];
        int digit;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (v > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(digit)) /
                    static_cast<uint64_t>(base)) return false;
        v = v * base + static_cast<uint64_t>(digit);
        any = true;
        ++pos;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Extract the hex string value of `key` (e.g. "data") and decode it
// into `out` bytes. Returns false if the key is absent or malformed.
bool extract_hex_bytes(std::string_view req, std::string_view key,
                       std::vector<uint8_t>& out) {
    JsonField field;
    if (!get_field(req, key, field) || field.kind != JsonField::Kind::String ||
        field.value.size() > 8192 || (field.value.size() & 1)) return false;
    auto hex_digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (std::size_t pos = 0; pos < field.value.size(); pos += 2) {
        int hi = hex_digit(field.value[pos]);
        int lo = hex_digit(field.value[pos + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool extract_string(std::string_view req, std::string_view key, std::string& out) {
    JsonField field;
    if (!get_field(req, key, field) || field.kind != JsonField::Kind::String) return false;
    out = field.value;
    return true;
}

bool field_exists(std::string_view req, std::string_view key) {
    JsonField ignored;
    return get_field(req, key, ignored);
}

// ─────────────────────────────────────────────────────────────────────
// mGBA driver
// ─────────────────────────────────────────────────────────────────────

struct OracleAudioTap {
    static constexpr std::size_t kMaxQueuedSamples = 262144;
    mAVStream stream{};
    std::vector<int16_t> mono;
    std::size_t read_index = 0;
    uint64_t samples_generated = 0;
    uint64_t samples_dropped = 0;
    unsigned sample_rate = 32768;

    OracleAudioTap() {
        stream.audioRateChanged = &OracleAudioTap::rate_changed;
        stream.postAudioFrame = &OracleAudioTap::post_audio_frame;
    }

    void clear_samples() {
        mono.clear();
        read_index = 0;
        samples_generated = 0;
        samples_dropped = 0;
    }

    std::vector<int16_t> drain(std::size_t max) {
        std::size_t available = mono.size() - read_index;
        std::size_t n = available < max ? available : max;
        std::vector<int16_t> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(mono[read_index + i]);
        }
        read_index += n;
        if (read_index > 65536 && read_index * 2 > mono.size()) {
            mono.erase(mono.begin(), mono.begin() +
                       static_cast<std::ptrdiff_t>(read_index));
            read_index = 0;
        }
        return out;
    }

    static void rate_changed(mAVStream* stream, unsigned rate) {
        auto* self = reinterpret_cast<OracleAudioTap*>(stream);
        self->sample_rate = rate;
    }

    static void post_audio_frame(mAVStream* stream, int16_t left, int16_t right) {
        auto* self = reinterpret_cast<OracleAudioTap*>(stream);
        int32_t mixed = (static_cast<int32_t>(left) +
                         static_cast<int32_t>(right)) / 2;
        ++self->samples_generated;
        if (self->mono.size() < kMaxQueuedSamples) {
            self->mono.push_back(static_cast<int16_t>(mixed));
        } else {
            ++self->samples_dropped;
        }
    }
};

struct Oracle {
    mCore*   core    = nullptr;
    bool skip_bios = false;
    bool rtc_tick_driven = false;
    int64_t rtc_epoch_ms = 946684800000LL;
    int64_t rtc_requested_epoch_ms = 946684800000LL;
    uint64_t frame_calls = 0;
    uint64_t video_frame_ends = 0;
    uint64_t key_reads = 0;
    uint64_t inst_count = 0;  // emu_step_inst calls since reset
    // mGBA renders the framebuffer into this buffer on each runFrame.
    // 240*160 pixels at BYTES_PER_PIXEL (default 4 = 32-bit ARGB).
    std::vector<uint32_t> video_buf;
    OracleAudioTap audio;
    std::vector<uint8_t> save_import;
    std::size_t save_import_expected = 0;
    std::size_t save_import_received = 0;
    std::vector<uint8_t> save_export;
    bool save_import_active = false;
    bool save_export_active = false;
    // Exactly one opaque mGBA core state, retained only for this process.
    std::vector<uint8_t> emulator_state;
    uint64_t emulator_state_id = 0;
    uint64_t emulator_state_sequence = 0;

    static constexpr std::size_t kMaxSaveBytes = 0x20000;
    static constexpr std::size_t kSaveChunkBytes = 2048;
    static constexpr std::size_t kMaxEmulatorStateBytes = 4 * 1024 * 1024;
    static constexpr uint64_t kMaxFrameBatch = 3600;

    static void frame_ended(void* context) {
        ++static_cast<Oracle*>(context)->video_frame_ends;
    }

    static void keys_read(void* context) {
        ++static_cast<Oracle*>(context)->key_reads;
    }

    void configure_rtc() {
        if (!core) return;
        core->rtc.override = rtc_tick_driven ? RTC_FAKE_EPOCH : RTC_FIXED;
        core->rtc.value = rtc_epoch_ms;
    }

    bool set_rtc(const std::string& mode, uint64_t epoch_ms) {
        if ((mode != "fixed" && mode != "tick") ||
            epoch_ms > 253402300799000ULL) return false;
        rtc_tick_driven = mode == "tick";
        uint64_t elapsed_ms = 0;
        if (rtc_tick_driven && core) {
            int32_t frequency = core->frequency(core);
            int32_t frame_cycles = core->frameCycles(core);
            if (frequency <= 0 || frame_cycles <= 0) return false;
            elapsed_ms = static_cast<uint64_t>(core->frameCounter(core)) *
                static_cast<uint64_t>(frame_cycles) * 1000ULL /
                static_cast<uint64_t>(frequency);
        }
        if (elapsed_ms > epoch_ms) return false;
        rtc_requested_epoch_ms = static_cast<int64_t>(epoch_ms);
        rtc_epoch_ms = static_cast<int64_t>(epoch_ms - elapsed_ms);
        configure_rtc();
        return true;
    }

    bool reset() {
        if (!core) return false;
        core->opts.skipBios = skip_bios;
        core->reset(core);
        if (!skip_bios) apply_real_hw_reset();
        rtc_epoch_ms = rtc_requested_epoch_ms;
        core->setKeys(core, 0);
        configure_rtc();
        frame_calls = 0;
        video_frame_ends = 0;
        key_reads = 0;
        inst_count = 0;
        audio.clear_samples();
        save_import_active = false;
        save_import.clear();
        save_import_expected = 0;
        save_import_received = 0;
        save_export_active = false;
        save_export.clear();
        return true;
    }

    bool import_save() {
        if (!core || !save_import_active ||
            save_import_received != save_import_expected) return false;
        VFile* vf = VFileMemChunk(save_import.data(), save_import.size());
        if (!vf) return false;
        if (!core->loadSave(core, vf)) {
            vf->close(vf);
            return false;
        }
        save_import_active = false;
        save_import.clear();
        save_import_expected = 0;
        save_import_received = 0;
        save_export.clear();
        save_export_active = false;
        return true;
    }

    bool begin_save_export() {
        if (!core) return false;
        void* bytes = nullptr;
        std::size_t size = core->savedataClone(core, &bytes);
        if (size > kMaxSaveBytes) { std::free(bytes); return false; }
        save_export.clear();
        if (size) {
            if (!bytes) return false;
            auto* data = static_cast<uint8_t*>(bytes);
            save_export.assign(data, data + size);
            std::free(bytes);
        }
        save_export_active = true;
        return true;
    }

    bool save_emulator_state() {
        if (!core) return false;
        const std::size_t size = core->stateSize(core);
        if (size == 0 || size > kMaxEmulatorStateBytes) return false;
        std::vector<uint8_t> next;
        try {
            next.resize(size);
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!core->saveState(core, next.data())) return false;
        emulator_state.swap(next);
        ++emulator_state_sequence;
        if (emulator_state_sequence == 0) ++emulator_state_sequence;
        emulator_state_id = emulator_state_sequence;
        return true;
    }

    bool load_emulator_state(uint64_t state_id) {
        if (!core || state_id == 0 || state_id != emulator_state_id ||
            emulator_state.empty() ||
            emulator_state.size() != core->stateSize(core)) return false;
        return core->loadState(core, emulator_state.data());
    }

    // Drive mGBA's post-reset CPU state to match real ARM7TDMI
    // hardware conventions (ARM ARM A2.6.5): PC=0, mode=Supervisor,
    // I=F=1, T=0. mGBA's GBAReset pre-bakes the SP banks then leaves
    // the CPU in System mode with I=F=0 — that mismatch skips the
    // first several BIOS instructions, breaking per-instruction
    // lockstep against our native runtime.
    //
    // The banked SP values mGBA programs (IRQ=0x03007FA0,
    // SVC=0x03007FE0, System=0x03007F00) already match our native
    // make_reset_cpu(); writing CPSR triggers _ARMReadCPSR which
    // re-applies the bank-swap, so the active R13 becomes the
    // mode's banked value automatically.
    void apply_real_hw_reset() {
        if (!core) return;
        int32_t cpsr = 0x000000D3;  // SVC=0x13, T=0, F=1, I=1, NZCV=0
        int32_t pc   = 0;
        core->writeRegister(core, "cpsr", &cpsr);
        core->writeRegister(core, "r15",  &pc);
    }

    // GBA-specific regions. Sizes per GBATEK § "GBA Memory Map".
    static constexpr uint32_t kBaseBIOS  = 0x00000000;
    static constexpr uint32_t kBaseEWRAM = 0x02000000;
    static constexpr uint32_t kBaseIWRAM = 0x03000000;
    static constexpr uint32_t kBaseIO    = 0x04000000;
    static constexpr uint32_t kBasePAL   = 0x05000000;
    static constexpr uint32_t kBaseVRAM  = 0x06000000;
    static constexpr uint32_t kBaseOAM   = 0x07000000;
    static constexpr uint32_t kBaseROM   = 0x08000000;

    static constexpr uint32_t kSizeBIOS  = 0x4000;
    static constexpr uint32_t kSizeEWRAM = 0x40000;
    static constexpr uint32_t kSizeIWRAM = 0x8000;
    static constexpr uint32_t kSizeIO    = 0x400;
    static constexpr uint32_t kSizePAL   = 0x400;
    static constexpr uint32_t kSizeVRAM  = 0x18000;
    static constexpr uint32_t kSizeOAM   = 0x400;

    bool init(const char* bios_path, const char* rom_path) {
        core = mCoreCreate(mPLATFORM_GBA);
        if (!core) {
            std::fprintf(stderr, "oracle: mCoreCreate failed\n");
            return false;
        }
        if (!core->init(core)) {
            std::fprintf(stderr, "oracle: core->init failed\n");
            return false;
        }
        mCoreInitConfig(core, nullptr);
        mCoreCallbacks callbacks{};
        callbacks.context = this;
        callbacks.videoFrameEnded = &Oracle::frame_ended;
        callbacks.keysRead = &Oracle::keys_read;
        core->addCoreCallbacks(core, &callbacks);

        if (bios_path && *bios_path) {
            VFile* vf = VFileOpen(bios_path, O_RDONLY);
            if (!vf) {
                std::fprintf(stderr, "oracle: open BIOS %s failed\n", bios_path);
                return false;
            }
            if (!core->loadBIOS(core, vf, 0)) {
                std::fprintf(stderr, "oracle: loadBIOS failed\n");
                return false;
            }
        }
        if (rom_path && *rom_path) {
            // mCoreLoadFile may derive a sibling save path. Never autoload it:
            // save import/export is explicit and process-local below.
            if (!mCoreLoadFile(core, rom_path)) {
                std::fprintf(stderr, "oracle: loadROM %s failed\n", rom_path);
                return false;
            }
        }

        // Provide a video buffer so the PPU renders into something
        // we can read back for emu_screenshot. Without this mGBA
        // skips rendering work entirely.
        unsigned w = 0, h = 0;
        core->desiredVideoDimensions(core, &w, &h);
        if (!w || !h || w > 512 || h > 512 ||
            static_cast<uint64_t>(w) * h > 512 * 512) {
            std::fprintf(stderr, "oracle: unsupported video dimensions %ux%u\n", w, h);
            return false;
        }
        video_buf.assign(static_cast<std::size_t>(w) * h, 0);
        core->setVideoBuffer(core, video_buf.data(), w);

        // With no BIOS and a cartridge present, ask mGBA to synthesize its
        // canonical post-BIOS handoff state. Do not overwrite that state with
        // apply_real_hw_reset(): doing so put PC back at zero in mGBA's HLE
        // BIOS image, so the old --no-bios comparison never reached the game.
        skip_bios =
            (!bios_path || !*bios_path) && rom_path && *rom_path;
        core->opts.skipBios = skip_bios;
        core->reset(core);
        if (!skip_bios) apply_real_hw_reset();
        configure_rtc();
        core->setAVStream(core, &audio.stream);
        audio.clear_samples();
        std::printf("oracle: mGBA core ready (bios=%s rom=%s video=%ux%u) "
                    "[boot=%s]\n",
                    bios_path ? bios_path : "(none)",
                    rom_path  ? rom_path  : "(none)", w, h,
                    skip_bios ? "HLE skip" :
                                "real ARM7TDMI reset: PC=0 SVC I=F=1");
        return true;
    }

    void shutdown() {
        if (core) {
            mCoreConfigDeinit(&core->config);
            core->deinit(core);
            core = nullptr;
        }
    }

    // One mCore::runFrame call. This call count is not a VBlank IRQ count.
    void step_frame() {
        if (!core) return;
        core->runFrame(core);
        ++frame_calls;
    }

    // Read `len` bytes starting at `base + off` via the bus. Returns
    // false if any byte falls outside the documented region.
    bool read_region(uint32_t base, uint32_t region_size,
                     uint64_t off, uint64_t len,
                     std::vector<uint8_t>& out) const {
        if (off > region_size || len > region_size || off + len > region_size) {
            return false;
        }
        out.resize(static_cast<std::size_t>(len));
        for (uint64_t i = 0; i < len; ++i) {
            out[static_cast<std::size_t>(i)] =
                static_cast<uint8_t>(core->busRead8(core, base + static_cast<uint32_t>(off + i)));
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────
// Command dispatch
// ─────────────────────────────────────────────────────────────────────

struct CmdResult {
    std::string body;
    bool ok = true;
};

void emit_error(std::string& out, const char* msg) {
    out = "{\"ok\":false,\"error\":\"";
    out += msg;
    out += "\"}";
}

void emit_ok_int(std::string& out, const char* key, uint64_t v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"%s\":%llu}", key,
                  static_cast<unsigned long long>(v));
    out = buf;
}

void cmd_read_region(Oracle& o, std::string_view req,
                     uint32_t base, uint32_t size, std::string& out) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    if (len > 4096) { emit_error(out, "read length exceeds 4096-byte limit"); return; }
    // Allow callers to pass either a region-relative offset or the
    // absolute bus address — both are convenient.
    uint64_t off = (addr >= base) ? addr - base : addr;
    std::vector<uint8_t> bytes;
    if (!o.read_region(base, size, off, len, bytes)) {
        emit_error(out, "out of range");
        return;
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u", static_cast<unsigned>(base + off));
    out += ab;
    out += ",\"len\":";
    char lb[32];
    std::snprintf(lb, sizeof(lb), "%llu",
                  static_cast<unsigned long long>(len));
    out += lb;
    out += ",\"data\":";
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

void cmd_audio_samples(Oracle& o, std::string_view req, std::string& out) {
    uint64_t max_samples = 4096;
    uint64_t parsed = 0;
    bool has_max = field_exists(req, "\"max\"");
    bool has_count = field_exists(req, "\"count\"");
    bool has_samples = field_exists(req, "\"samples\"");
    if ((static_cast<unsigned>(has_max) + static_cast<unsigned>(has_count) +
         static_cast<unsigned>(has_samples)) > 1) {
        emit_error(out, "specify only one audio sample count field"); return;
    }
    if ((has_max && !extract_uint(req, "\"max\"", parsed)) ||
        (has_count && !extract_uint(req, "\"count\"", parsed)) ||
        (has_samples && !extract_uint(req, "\"samples\"", parsed))) {
        emit_error(out, "invalid audio sample count"); return;
    }
    if (has_max || has_count || has_samples) max_samples = parsed;
    if (max_samples > 16384) {
        emit_error(out, "audio sample count exceeds 16384-sample limit"); return;
    }

    std::vector<int16_t> samples =
        o.audio.drain(static_cast<std::size_t>(max_samples));
    std::vector<uint8_t> bytes(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        uint16_t u = static_cast<uint16_t>(samples[i]);
        bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
        bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,"
                  "\"samples_generated\":%llu,\"samples_dropped\":%llu,\"data\":",
                  static_cast<unsigned>(o.audio.sample_rate),
                  static_cast<unsigned long long>(samples.size()),
                  static_cast<unsigned long long>(
                      o.audio.samples_generated),
                  static_cast<unsigned long long>(o.audio.samples_dropped));
    out = hdr;
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

void dispatch(Oracle& o, std::string_view req, std::string& out) {
    out.clear();

    if (req == "ping") {
        out = "{\"ok\":true,\"who\":\"gbarecomp_oracle\"}";
        return;
    }
    if (req == "frame") {
        emit_ok_int(out, "frame", o.frame_calls);
        return;
    }
    JsonObject fields;
    std::vector<std::string> field_order;
    if (!parse_json_object(req, fields, &field_order) || field_order.empty()) {
        emit_error(out, "malformed JSON object");
        return;
    }
    std::string command = field_order.front();
    auto command_field = fields.find("cmd");
    if (command_field != fields.end()) {
        if (command_field->second.kind != JsonField::Kind::String ||
            command_field->second.value.empty()) {
            emit_error(out, "cmd must be a non-empty string"); return;
        }
        command = command_field->second.value;
    }
    auto starts = [&](std::string_view tok) {
        return tok.size() >= 2 && command == tok.substr(1, tok.size() - 2);
    };

    if (starts("\"ping\"")) {
        out = "{\"ok\":true,\"who\":\"gbarecomp_oracle\"}";
        return;
    }
    if (starts("\"frame\"")) {
        emit_ok_int(out, "frame", o.frame_calls);
        return;
    }
    if (starts("\"emu_capabilities\"")) {
        char body[512];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"protocol\":2,\"oracle\":\"gbarecomp_oracle\","
            "\"mgba\":\"%s\",\"mgba_commit\":\"%s\","
            "\"reset_modes\":[\"bios\",\"hle\"],"
            "\"rtc_modes\":[\"fixed\",\"tick\"],\"positive_key_mask\":1023,"
            "\"max_request_bytes\":8192,\"max_memory_read_bytes\":4096,"
            "\"max_audio_capture_samples\":16384,\"max_frame_batch\":%llu,"
            "\"max_audio_queue_samples\":%llu,\"max_save_bytes\":%llu,"
            "\"save_chunk_bytes\":%llu,\"max_emulator_state_bytes\":%llu,"
            "\"emulator_state_slots\":1,\"state_api\":\"mcore-in-memory-single-slot-v1\"}",
            MGBA_ORACLE_VERSION, MGBA_ORACLE_COMMIT,
            static_cast<unsigned long long>(Oracle::kMaxFrameBatch),
            static_cast<unsigned long long>(OracleAudioTap::kMaxQueuedSamples),
            static_cast<unsigned long long>(Oracle::kMaxSaveBytes),
            static_cast<unsigned long long>(Oracle::kSaveChunkBytes),
            static_cast<unsigned long long>(Oracle::kMaxEmulatorStateBytes));
        out = body;
        return;
    }
    if (starts("\"emu_phase\"")) {
        char body[384];
        uint32_t vcount = o.core ? o.core->busRead16(o.core, Oracle::kBaseIO + 6) : 0;
        uint32_t core_frame = o.core ? o.core->frameCounter(o.core) : 0;
        uint32_t keys = o.core ? o.core->getKeys(o.core) : 0;
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"run_frame_calls\":%llu,\"core_frame_counter\":%u,"
            "\"video_frame_end_events\":%llu,\"key_reads\":%llu,\"keys_pressed\":%u,"
            "\"vcount\":%u,\"reset_mode\":\"%s\"}",
            static_cast<unsigned long long>(o.frame_calls), core_frame,
            static_cast<unsigned long long>(o.video_frame_ends),
            static_cast<unsigned long long>(o.key_reads), keys, vcount,
            o.skip_bios ? "hle" : "bios");
        out = body;
        return;
    }
    if (starts("\"emu_state_save\"")) {
        if (!o.save_emulator_state()) {
            emit_error(out, "mGBA state save failed or exceeded the 4 MiB limit");
            return;
        }
        char body[128];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"state_id\":%llu,\"size\":%llu}",
            static_cast<unsigned long long>(o.emulator_state_id),
            static_cast<unsigned long long>(o.emulator_state.size()));
        out = body;
        return;
    }
    if (starts("\"emu_state_load\"")) {
        uint64_t state_id = 0;
        if (!extract_uint(req, "\"state_id\"", state_id)) {
            emit_error(out, "missing/invalid state_id");
            return;
        }
        if (!o.load_emulator_state(state_id)) {
            emit_error(out, "state_id is unavailable or mGBA state load failed");
            return;
        }
        out = "{\"ok\":true}";
        return;
    }
    if (starts("\"emu_rtc_state\"")) {
        uint64_t rtc_seconds = o.core && o.core->rtc.d.unixTime ?
            static_cast<uint64_t>(o.core->rtc.d.unixTime(&o.core->rtc.d)) : 0;
        char body[256];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"mode\":\"%s\",\"epoch_seconds\":%llu,"
            "\"core_frame_counter\":%u,\"run_frame_calls\":%llu}",
            o.rtc_tick_driven ? "tick" : "fixed",
            static_cast<unsigned long long>(rtc_seconds),
            o.core ? o.core->frameCounter(o.core) : 0,
            static_cast<unsigned long long>(o.frame_calls));
        out = body;
        return;
    }
    if (starts("\"emu_set_rtc\"")) {
        std::string mode;
        uint64_t epoch_ms = 0;
        if (!extract_string(req, "\"mode\"", mode) ||
            !extract_uint(req, "\"epoch_ms\"", epoch_ms)) {
            emit_error(out, "missing/invalid mode or epoch_ms"); return;
        }
        if (!o.set_rtc(mode, epoch_ms)) {
            emit_error(out, "unsupported RTC mode or epoch"); return;
        }
        out = "{\"ok\":true}";
        return;
    }
    if (starts("\"emu_save_import_begin\"")) {
        uint64_t size = 0;
        if (!extract_uint(req, "\"size\"", size)) {
            emit_error(out, "missing save size"); return;
        }
        if (size != 512 && size != 8192 && size != 32768 &&
            size != 65536 && size != 131072) {
            emit_error(out, "unsupported cartridge save size"); return;
        }
        o.save_import.assign(static_cast<std::size_t>(size), 0xFF);
        o.save_import_expected = static_cast<std::size_t>(size);
        o.save_import_received = 0;
        o.save_import_active = true;
        o.save_export.clear();
        o.save_export_active = false;
        emit_ok_int(out, "import_size", size);
        return;
    }
    if (starts("\"emu_save_import_chunk\"")) {
        uint64_t offset = 0;
        std::vector<uint8_t> bytes;
        if (!extract_uint(req, "\"offset\"", offset) ||
            !extract_hex_bytes(req, "\"data\"", bytes)) {
            emit_error(out, "missing/invalid import offset or data"); return;
        }
        if (!o.save_import_active || bytes.empty() ||
            bytes.size() > Oracle::kSaveChunkBytes ||
            offset != o.save_import_received ||
            o.save_import_received > o.save_import_expected ||
            bytes.size() > o.save_import_expected - o.save_import_received) {
            emit_error(out, "invalid import chunk sequence"); return;
        }
        std::copy(bytes.begin(), bytes.end(), o.save_import.begin() +
                  static_cast<std::ptrdiff_t>(o.save_import_received));
        o.save_import_received += bytes.size();
        emit_ok_int(out, "next_offset", o.save_import_received);
        return;
    }
    if (starts("\"emu_save_import_commit\"")) {
        if (!o.import_save()) { emit_error(out, "save import incomplete or failed"); return; }
        out = "{\"ok\":true}";
        return;
    }
    if (starts("\"emu_save_export_begin\"")) {
        if (!o.begin_save_export()) { emit_error(out, "save export failed"); return; }
        char body[160];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"size\":%llu,\"chunk_bytes\":%llu,\"format\":\"raw-cartridge-save\"}",
            static_cast<unsigned long long>(o.save_export.size()),
            static_cast<unsigned long long>(Oracle::kSaveChunkBytes));
        out = body;
        return;
    }
    if (starts("\"emu_save_export_chunk\"")) {
        uint64_t offset = 0, count = 0;
        if (!extract_uint(req, "\"offset\"", offset) ||
            !extract_uint(req, "\"count\"", count)) {
            emit_error(out, "missing export offset or count"); return;
        }
        if (!o.save_export_active || !count || count > Oracle::kSaveChunkBytes ||
            offset > o.save_export.size() || count > o.save_export.size() - offset) {
            emit_error(out, "invalid export chunk range"); return;
        }
        out = "{\"ok\":true,\"offset\":" + std::to_string(offset) + ",\"data\":";
        json_emit_hex(out, o.save_export.data() + offset, static_cast<std::size_t>(count));
        out += "}";
        return;
    }
    if (starts("\"emu_save_export_end\"")) {
        o.save_export.clear();
        o.save_export_active = false;
        out = "{\"ok\":true}";
        return;
    }
    if (starts("\"emu_vblank_count\"")) {
        emit_error(out, "deprecated; use emu_phase for distinct frame observations");
        return;
    }
    if (starts("\"emu_step_inst\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t before_cycles =
            o.core->timing ? mTimingCurrentTime(o.core->timing) : 0;
        o.core->step(o.core);
        uint64_t after_cycles =
            o.core->timing ? mTimingCurrentTime(o.core->timing) : 0;
        uint64_t step_cycles = after_cycles - before_cycles;
        ++o.inst_count;
        int32_t pc = 0, cpsr = 0;
        o.core->readRegister(o.core, "r15",  &pc);
        o.core->readRegister(o.core, "cpsr", &cpsr);
        bool thumb = (cpsr & (1u << 5)) != 0;
        std::string body;
        body.reserve(512);
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
            "{\"ok\":true,\"pc\":%u,\"thumb\":%s,\"run_frame_calls\":%llu,"
            "\"cycles\":%llu,\"cycles_elapsed\":%llu",
            static_cast<unsigned>(pc),
            thumb ? "true" : "false",
            static_cast<unsigned long long>(o.frame_calls),
            static_cast<unsigned long long>(step_cycles),
            static_cast<unsigned long long>(after_cycles));
        body = hdr;
        for (int i = 0; i < 15; ++i) {
            char name[8]; std::snprintf(name, sizeof(name), "r%d", i);
            int32_t v = 0;
            o.core->readRegister(o.core, name, &v);
            char f[48];
            std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                          i, static_cast<unsigned>(v));
            body += f;
        }
        char cf[48];
        std::snprintf(cf, sizeof(cf), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += cf;
        out = body;
        return;
    }
    if (starts("\"emu_run_until_pc\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t target = 0;
        uint64_t max_steps = 1'000'000;
        if (!extract_uint(req, "\"target\"", target)) {
            emit_error(out, "missing target");
            return;
        }
        if (field_exists(req, "\"max_steps\"") &&
            !extract_uint(req, "\"max_steps\"", max_steps)) {
            emit_error(out, "invalid max_steps"); return;
        }
        if (target > UINT32_MAX || max_steps > 1'000'000) { emit_error(out, "target or max_steps exceeds limit"); return; }
        for (uint64_t i = 0; i <= max_steps; ++i) {
            int32_t raw_pc = 0, cpsr = 0;
            o.core->readRegister(o.core, "r15", &raw_pc);
            o.core->readRegister(o.core, "cpsr", &cpsr);
            const bool thumb = (static_cast<uint32_t>(cpsr) & (1u << 5)) != 0;
            const uint32_t bias = thumb ? 2u : 4u;
            const uint32_t exec_pc = static_cast<uint32_t>(raw_pc) - bias;
            if (exec_pc == static_cast<uint32_t>(target)) {
                char body[256];
                std::snprintf(
                    body, sizeof(body),
                    "{\"ok\":true,\"found\":true,\"steps\":%llu,"
                    "\"pc\":%u,\"raw_pc\":%u,\"cpsr\":%u,"
                    "\"cycles_elapsed\":%llu}",
                    static_cast<unsigned long long>(i),
                    static_cast<unsigned>(exec_pc),
                    static_cast<unsigned>(raw_pc),
                    static_cast<unsigned>(cpsr),
                    static_cast<unsigned long long>(
                        o.core->timing ? mTimingCurrentTime(o.core->timing) : 0));
                out = body;
                return;
            }
            if (i == max_steps) break;
            o.core->step(o.core);
            ++o.inst_count;
        }
        char body[160];
        std::snprintf(
            body, sizeof(body),
            "{\"ok\":true,\"found\":false,\"steps\":%llu}",
            static_cast<unsigned long long>(max_steps));
        out = body;
        return;
    }
    if (starts("\"emu_find_dma_source_reset\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t channel_arg = 2;
        uint64_t max_steps = 1'000'000;
        if ((field_exists(req, "\"channel\"") && !extract_uint(req, "\"channel\"", channel_arg)) ||
            (field_exists(req, "\"max_steps\"") && !extract_uint(req, "\"max_steps\"", max_steps)) ||
            channel_arg > 3 || max_steps == 0 || max_steps > 1'000'000) {
            emit_error(out, "invalid channel/max_steps");
            return;
        }
        auto* gba = static_cast<GBA*>(o.core->board);
        const int ch = static_cast<int>(channel_arg);
        uint32_t previous = gba->memory.dma[ch].nextSource;
        for (uint64_t i = 1; i <= max_steps; ++i) {
            o.core->step(o.core);
            const GBADMA& dma = gba->memory.dma[ch];
            if (dma.nextSource < previous) {
                int32_t pc = 0, cpsr = 0;
                o.core->readRegister(o.core, "r15", &pc);
                o.core->readRegister(o.core, "cpsr", &cpsr);
                char body[512];
                std::snprintf(
                    body, sizeof(body),
                    "{\"ok\":true,\"found\":true,\"steps\":%llu,"
                    "\"time\":%u,\"pc\":%u,\"cpsr\":%u,"
                    "\"previous\":%u,\"next_source\":%u,"
                    "\"source\":%u,\"dest\":%u,\"reg\":%u,"
                    "\"count\":%d,\"next_count\":%d}",
                    static_cast<unsigned long long>(i),
                    static_cast<unsigned>(
                        mTimingCurrentTime(&gba->timing)),
                    static_cast<unsigned>(pc),
                    static_cast<unsigned>(cpsr),
                    static_cast<unsigned>(previous),
                    static_cast<unsigned>(dma.nextSource),
                    static_cast<unsigned>(dma.source),
                    static_cast<unsigned>(dma.dest),
                    static_cast<unsigned>(dma.reg),
                    static_cast<int>(dma.count),
                    static_cast<int>(dma.nextCount));
                out = body;
                return;
            }
            previous = dma.nextSource;
        }
        char body[192];
        std::snprintf(
            body, sizeof(body),
            "{\"ok\":true,\"found\":false,\"steps\":%llu,"
            "\"next_source\":%u}",
            static_cast<unsigned long long>(max_steps),
            static_cast<unsigned>(gba->memory.dma[ch].nextSource));
        out = body;
        return;
    }
    if (starts("\"emu_step_frames\"")) {
        uint64_t count = 0;
        if (!extract_uint(req, "\"count\"", count) || count > Oracle::kMaxFrameBatch) {
            emit_error(out, "invalid count");
            return;
        }
        for (uint64_t i = 0; i < count; ++i) o.step_frame();
        out = "{\"ok\":true,\"run_frame_calls\":" + std::to_string(o.frame_calls) +
              ",\"frame\":" + std::to_string(o.frame_calls) + "}";
        return;
    }
    if (starts("\"emu_step\"") || starts("\"emu_step_to_vblank\"")) {
        o.step_frame();
        out = "{\"ok\":true,\"run_frame_calls\":" + std::to_string(o.frame_calls) +
              ",\"frame\":" + std::to_string(o.frame_calls) + "}";
        return;
    }
    if (starts("\"emu_reset\"")) {
        if (!o.reset()) { emit_error(out, "reset failed"); return; }
        out = std::string("{\"ok\":true,\"reset_mode\":\"") +
            (o.skip_bios ? "hle" : "bios") + "\"}";
        return;
    }
    if (starts("\"read_emu_oam\"")) {
        cmd_read_region(o, req, Oracle::kBaseOAM, Oracle::kSizeOAM, out);
        return;
    }
    if (starts("\"read_emu_pal\"")) {
        cmd_read_region(o, req, Oracle::kBasePAL, Oracle::kSizePAL, out);
        return;
    }
    if (starts("\"read_emu_vram\"")) {
        cmd_read_region(o, req, Oracle::kBaseVRAM, Oracle::kSizeVRAM, out);
        return;
    }
    if (starts("\"read_emu_iwram\"")) {
        cmd_read_region(o, req, Oracle::kBaseIWRAM, Oracle::kSizeIWRAM, out);
        return;
    }
    if (starts("\"read_emu_ewram\"")) {
        cmd_read_region(o, req, Oracle::kBaseEWRAM, Oracle::kSizeEWRAM, out);
        return;
    }
    if (starts("\"read_emu_io\"")) {
        cmd_read_region(o, req, Oracle::kBaseIO, Oracle::kSizeIO, out);
        return;
    }
    if (starts("\"read_emu_rom\"")) {
        // ROM cartridge: cap reads at 32 MB so out-of-range queries
        // don't iterate forever.
        cmd_read_region(o, req, Oracle::kBaseROM, 0x02000000u, out);
        return;
    }
    if (starts("\"emu_matrix_state\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        const GBAMatrix& matrix = gba->memory.matrix;
        char header[256];
        std::snprintf(
            header, sizeof(header),
            "{\"ok\":true,\"command\":%u,\"physical\":%u,"
            "\"virtual\":%u,\"size\":%u,\"mappings\":[",
            static_cast<unsigned>(matrix.cmd),
            static_cast<unsigned>(matrix.paddr),
            static_cast<unsigned>(matrix.vaddr),
            static_cast<unsigned>(matrix.size));
        out = header;
        for (int i = 0; i < GBA_MATRIX_MAPPINGS_MAX; ++i) {
            char value[32];
            std::snprintf(value, sizeof(value), "%s%u",
                          i == 0 ? "" : ",",
                          static_cast<unsigned>(matrix.mappings[i]));
            out += value;
        }
        out += "]}";
        return;
    }
    if (starts("\"emu_registers\"")) {
        // mGBA exposes registers via readRegister(name, &out). ARM7TDMI
        // names: "r0".."r15" and "cpsr". Anything missing comes back
        // as 0 with ok=false on the underlying call.
        std::string body = "{\"ok\":true";
        for (int r = 0; r <= 15; ++r) {
            char name[8]; std::snprintf(name, sizeof(name), "r%d", r);
            int32_t v = 0;
            o.core->readRegister(o.core, name, &v);
            char field[64];
            std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                          r, static_cast<unsigned>(v));
            body += field;
        }
        int32_t cpsr = 0;
        o.core->readRegister(o.core, "cpsr", &cpsr);
        char fb[64];
        std::snprintf(fb, sizeof(fb), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += fb;
        out = body;
        return;
    }
    if (starts("\"emu_pc\"")) {
        int32_t pc = 0;
        o.core->readRegister(o.core, "r15", &pc);
        emit_ok_int(out, "pc", static_cast<uint32_t>(pc));
        return;
    }
    if (starts("\"emu_inst_count\"")) {
        emit_ok_int(out, "inst_count", o.inst_count);
        return;
    }

    auto io16 = [&](uint32_t off) -> uint16_t {
        return static_cast<uint16_t>(
            o.core->busRead16(o.core, 0x04000000u + off));
    };
    auto io32 = [&](uint32_t off) -> uint32_t {
        uint32_t lo = io16(off);
        uint32_t hi = io16(off + 2);
        return lo | (hi << 16);
    };
    auto append_kv = [](std::string& s, const char* key, uint64_t v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"%s\":%llu",
                      key, static_cast<unsigned long long>(v));
        s += buf;
    };

    if (starts("\"emu_ppu_state\"")) {
        std::string body = "{\"ok\":true";
        append_kv(body, "dispcnt",  io16(0x00));
        append_kv(body, "dispstat", io16(0x04));
        append_kv(body, "vcount",   io16(0x06));
        for (int bg = 0; bg < 4; ++bg) {
            char k[24];
            std::snprintf(k, sizeof(k), "bg%dcnt", bg);
            append_kv(body, k, io16(0x08 + bg * 2));
            std::snprintf(k, sizeof(k), "bg%dhofs", bg);
            append_kv(body, k, io16(0x10 + bg * 4));
            std::snprintf(k, sizeof(k), "bg%dvofs", bg);
            append_kv(body, k, io16(0x12 + bg * 4));
        }
        // BG2/BG3 affine: PA/PB/PC/PD + reference X/Y.
        append_kv(body, "bg2pa", io16(0x20)); append_kv(body, "bg2pb", io16(0x22));
        append_kv(body, "bg2pc", io16(0x24)); append_kv(body, "bg2pd", io16(0x26));
        append_kv(body, "bg2x",  io32(0x28)); append_kv(body, "bg2y",  io32(0x2C));
        append_kv(body, "bg3pa", io16(0x30)); append_kv(body, "bg3pb", io16(0x32));
        append_kv(body, "bg3pc", io16(0x34)); append_kv(body, "bg3pd", io16(0x36));
        append_kv(body, "bg3x",  io32(0x38)); append_kv(body, "bg3y",  io32(0x3C));
        // Windows.
        append_kv(body, "win0h", io16(0x40)); append_kv(body, "win1h", io16(0x42));
        append_kv(body, "win0v", io16(0x44)); append_kv(body, "win1v", io16(0x46));
        append_kv(body, "winin", io16(0x48)); append_kv(body, "winout",io16(0x4A));
        // Mosaic + blending.
        append_kv(body, "mosaic",  io16(0x4C));
        append_kv(body, "bldcnt",  io16(0x50));
        append_kv(body, "bldalpha",io16(0x52));
        append_kv(body, "bldy",    io16(0x54));
        body += "}";
        out = body;
        return;
    }

    if (starts("\"emu_dma_state\"")) {
        std::string body = "{\"ok\":true,\"channels\":[";
        for (int ch = 0; ch < 4; ++ch) {
            if (ch) body += ",";
            uint32_t base = 0xB0u + ch * 12u;
            uint32_t sad   = io32(base);
            uint32_t dad   = io32(base + 4);
            uint16_t cnt_l = io16(base + 8);
            uint16_t cnt_h = io16(base + 10);
            char chunk[256];
            std::snprintf(chunk, sizeof(chunk),
                "{\"ch\":%d,\"sad\":%u,\"dad\":%u,\"cnt_l\":%u,\"cnt_h\":%u,"
                "\"enable\":%d,\"start_mode\":%d}",
                ch,
                static_cast<unsigned>(sad), static_cast<unsigned>(dad),
                static_cast<unsigned>(cnt_l), static_cast<unsigned>(cnt_h),
                (cnt_h >> 15) & 1, (cnt_h >> 12) & 3);
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_dma_internal\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        std::string body = "{\"ok\":true,\"channels\":[";
        for (int ch = 0; ch < 4; ++ch) {
            if (ch) body += ",";
            const GBADMA& dma = gba->memory.dma[ch];
            char chunk[320];
            std::snprintf(chunk, sizeof(chunk),
                "{\"ch\":%d,\"reg\":%u,\"source\":%u,\"dest\":%u,"
                "\"count\":%d,\"nextSource\":%u,\"nextDest\":%u,"
                "\"nextCount\":%d,\"when\":%u}",
                ch,
                static_cast<unsigned>(dma.reg),
                static_cast<unsigned>(dma.source),
                static_cast<unsigned>(dma.dest),
                static_cast<int>(dma.count),
                static_cast<unsigned>(dma.nextSource),
                static_cast<unsigned>(dma.nextDest),
                static_cast<int>(dma.nextCount),
                static_cast<unsigned>(dma.when));
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_timer_state\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        std::string body = "{\"ok\":true,\"timers\":[";
        for (int t = 0; t < 4; ++t) {
            if (t) body += ",";
            uint32_t base = 0x100u + t * 4u;
            uint16_t count_or_reload = io16(base);
            uint16_t cnt_h           = io16(base + 2);
            char chunk[256];
            std::snprintf(chunk, sizeof(chunk),
                "{\"t\":%d,\"counter\":%u,\"cnt_h\":%u,"
                "\"enable\":%d,\"cascade\":%d,\"prescaler\":%d,"
                "\"reload\":%u,\"last_event\":%d,\"flags\":%u}",
                t, count_or_reload, cnt_h,
                (cnt_h >> 7) & 1, (cnt_h >> 2) & 1, cnt_h & 3,
                static_cast<unsigned>(gba->timers[t].reload),
                static_cast<int>(gba->timers[t].lastEvent),
                static_cast<unsigned>(gba->timers[t].flags));
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_audio_state\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        const GBAAudio& audio = gba->audio;
        std::string body;
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr),
            "{\"ok\":true,\"time\":%u,\"rate\":%u,"
            "\"samples_generated\":%llu,\"soundbias\":%u,"
            "\"sampleInterval\":%d,\"sampleIndex\":%d,"
            "\"lastSample\":%d,\"nextSample\":%d",
            static_cast<unsigned>(mTimingCurrentTime(&gba->timing)),
            static_cast<unsigned>(o.audio.sample_rate),
            static_cast<unsigned long long>(o.audio.samples_generated),
            static_cast<unsigned>(audio.soundbias),
            static_cast<int>(audio.sampleInterval),
            static_cast<int>(audio.sampleIndex),
            static_cast<int>(audio.lastSample),
            static_cast<int>(
                mTimingUntil(&gba->timing, &audio.sampleEvent)));
        body = hdr;
        auto append_fifo = [&](const char* name, const GBAAudioFIFO& fifo) {
            char fh[192];
            int fifo_size = fifo.fifoWrite >= fifo.fifoRead
                ? fifo.fifoWrite - fifo.fifoRead
                : GBA_AUDIO_FIFO_SIZE - fifo.fifoRead + fifo.fifoWrite;
            std::snprintf(fh, sizeof(fh),
                ",\"%s\":{\"write\":%d,\"read\":%d,\"count\":%d,"
                "\"internalSample\":%u,\"internalRemaining\":%d,"
                "\"samples\":[",
                name, fifo.fifoWrite, fifo.fifoRead, fifo_size,
                static_cast<unsigned>(fifo.internalSample),
                fifo.internalRemaining);
            body += fh;
            for (int i = 0; i < GBA_MAX_SAMPLES; ++i) {
                if (i) body += ",";
                char b[16];
                std::snprintf(b, sizeof(b), "%d",
                              static_cast<int>(fifo.samples[i]));
                body += b;
            }
            body += "]}";
        };
        append_fifo("fifo_a", audio.chA);
        append_fifo("fifo_b", audio.chB);
        body += "}";
        out = body;
        return;
    }

    if (starts("\"emu_screenshot\"")) {
        // mGBA renders 32-bit color_t into video_buf. In the default
        // 32-bit build, mGBA's native color layout is 0x00BBGGRR
        // (M_COLOR_RED = 0x000000FF). Convert to RGB888 so native and
        // oracle screenshots can be compared byte-for-byte.
        const void* px = nullptr;
        std::size_t stride = 0;  // pixels per row
        o.core->getPixels(o.core, &px, &stride);
        if (!px || stride < 240) {
            emit_error(out, "no framebuffer (mGBA not rendering)");
            return;
        }
        const uint32_t* src = static_cast<const uint32_t*>(px);
        std::vector<uint8_t> rgb(240 * 160 * 3);
        for (uint32_t y = 0; y < 160; ++y) {
            for (uint32_t x = 0; x < 240; ++x) {
                uint32_t p = src[y * stride + x];
                uint8_t r = (p >>  0) & 0xFFu;
                uint8_t g = (p >>  8) & 0xFFu;
                uint8_t b = (p >> 16) & 0xFFu;
                uint8_t* d = rgb.data() + (y * 240 + x) * 3;
                d[0] = r; d[1] = g; d[2] = b;
            }
        }
        out = "{\"ok\":true,\"w\":240,\"h\":160,\"data\":";
        json_emit_hex(out, rgb.data(), rgb.size());
        out += "}";
        return;
    }

    if (starts("\"emu_audio_samples\"")) {
        cmd_audio_samples(o, req, out);
        return;
    }

    if (starts("\"emu_irq_state\"")) {
        uint16_t ie  = io16(0x200);
        uint16_t if_ = io16(0x202);
        uint16_t ime = io16(0x208);
        char body[160];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"ie\":%u,\"if\":%u,\"ime\":%u,\"pending\":%u}",
            ie, if_, ime, static_cast<unsigned>(ie & if_));
        out = body;
        return;
    }
    // ── State-injection bridge ──────────────────────────────────────
    // These let a harness sync mGBA to a native gbarecomp savestate:
    // read the native runtime's RAM + regs over its TCP server, then
    // push them here. Sync point is a frame boundary; mGBA then runs
    // forward and we diff its state against native (find-first-divergence
    // for sound/transition bugs like MC-HP-002/003).

    if (starts("\"emu_set_keys\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t keys = 0;
        if (!extract_uint(req, "\"keys\"", keys)) {
            emit_error(out, "missing/invalid positive pressed-key mask"); return;
        }
        if (keys > 0x3FF) { emit_error(out, "pressed-key mask exceeds 10 GBA keys"); return; }
        o.core->setKeys(o.core, static_cast<uint32_t>(keys));
        emit_ok_int(out, "keys_pressed", keys);
        return;
    }

    if (starts("\"emu_write\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t addr = 0;
        if (!extract_uint(req, "\"addr\"", addr)) {
            emit_error(out, "missing addr");
            return;
        }
        std::vector<uint8_t> bytes;
        if (!extract_hex_bytes(req, "\"data\"", bytes)) {
            emit_error(out, "missing/odd data");
            return;
        }
        if (bytes.empty() || bytes.size() > 2048 || addr > UINT32_MAX ||
            bytes.size() - 1 > UINT32_MAX - addr) {
            emit_error(out, "write address/data exceeds limits"); return;
        }
        uint64_t width = 8;
        if (field_exists(req, "\"width\"") &&
            !extract_uint(req, "\"width\"", width)) {
            emit_error(out, "invalid write width"); return;
        }
        if ((width != 8 && width != 16 && width != 32) ||
            (width == 16 && (addr & 1)) || (width == 32 && (addr & 3)) ||
            (width == 16 && bytes.size() % 2) || (width == 32 && bytes.size() % 4)) {
            emit_error(out, "unsupported or misaligned write width/data"); return;
        }
        uint32_t a = static_cast<uint32_t>(addr);
        if (width == 16 && (bytes.size() % 2) == 0) {
            for (std::size_t i = 0; i < bytes.size(); i += 2) {
                uint16_t v = static_cast<uint16_t>(
                    bytes[i] | (bytes[i + 1] << 8));
                o.core->busWrite16(o.core, a + static_cast<uint32_t>(i), v);
            }
        } else if (width == 32 && (bytes.size() % 4) == 0) {
            for (std::size_t i = 0; i < bytes.size(); i += 4) {
                uint32_t v = static_cast<uint32_t>(bytes[i]) |
                    (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                    (static_cast<uint32_t>(bytes[i + 2]) << 16) |
                    (static_cast<uint32_t>(bytes[i + 3]) << 24);
                o.core->busWrite32(o.core, a + static_cast<uint32_t>(i), v);
            }
        } else if (width == 8) {
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                o.core->busWrite8(o.core, a + static_cast<uint32_t>(i),
                                  bytes[i]);
            }
        }
        emit_ok_int(out, "wrote", bytes.size());
        return;
    }

    if (starts("\"emu_set_regs\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        // Write CPSR first so the mode (and thus the active r13/r14
        // bank) is correct before the GPRs land; r15 last so the PC
        // sticks after any bank swap. Only keys that are present are
        // written, so a partial reg set is allowed.
        uint64_t values[17]{};
        bool present[17]{};
        if (field_exists(req, "\"cpsr\"")) {
            if (!extract_uint(req, "\"cpsr\"", values[0]) || values[0] > UINT32_MAX) {
                emit_error(out, "invalid cpsr"); return;
            }
            present[0] = true;
        }
        for (int i = 0; i <= 15; ++i) {
            char key[12];
            std::snprintf(key, sizeof(key), "\"r%d\"", i);
            if (field_exists(req, key)) {
                if (!extract_uint(req, key, values[i + 1]) || values[i + 1] > UINT32_MAX) {
                    emit_error(out, "invalid register value"); return;
                }
                present[i + 1] = true;
            }
        }
        if (present[0]) {
            int32_t cpsr = static_cast<int32_t>(values[0]);
            o.core->writeRegister(o.core, "cpsr", &cpsr);
        }
        for (int i = 0; i <= 15; ++i) if (present[i + 1]) {
            int32_t rv = static_cast<int32_t>(values[i + 1]);
            char name[8];
            std::snprintf(name, sizeof(name), "r%d", i);
            o.core->writeRegister(o.core, name, &rv);
        }
        out = "{\"ok\":true}";
        return;
    }

    if (starts("\"quit\"")) {
        out = "{\"ok\":true,\"bye\":true}";
        return;
    }

    emit_error(out, "unknown command");
}

// ─────────────────────────────────────────────────────────────────────
// Line-delimited TCP server
// ─────────────────────────────────────────────────────────────────────

bool socket_send_all(socket_t socket, const char* data, std::size_t size) {
    std::size_t sent_total = 0;
    while (sent_total < size) {
        std::size_t remaining = size - sent_total;
#ifdef _WIN32
        int chunk = static_cast<int>(remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining);
#else
        std::size_t chunk = remaining;
#endif
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        int sent = ::send(socket, data + sent_total, chunk, flags);
        if (sent <= 0) return false;
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

void set_socket_deadlines(socket_t socket) {
#ifdef _WIN32
    DWORD timeout_ms = 30000;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval timeout{};
    timeout.tv_sec = 30;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

int run_server(Oracle& o, int port, const char* bind_address) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "oracle: WSAStartup failed\n");
        return 1;
    }
#endif

    socket_t srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::fprintf(stderr, "oracle: socket() failed\n");
        return 1;
    }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (!bind_address || std::strcmp(bind_address, "127.0.0.1") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (std::strcmp(bind_address, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        std::fprintf(stderr, "oracle: unsupported bind address %s\n", bind_address);
        CLOSESOCK(srv);
        return 1;
    }
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "oracle: bind to %s:%d failed\n", bind_address, port);
        CLOSESOCK(srv);
        return 1;
    }
    if (::listen(srv, 1) != 0) {
        std::fprintf(stderr, "oracle: listen failed\n");
        CLOSESOCK(srv);
        return 1;
    }

    std::printf("oracle: listening on %s:%d\n", bind_address, port);
    std::fflush(stdout);

    bool quit_server = false;
    while (!quit_server) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        socket_t c = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c == INVALID_SOCKET) continue;
        set_socket_deadlines(c);

        std::string inbuf;
        std::string resp;
        char rbuf[4096];
        bool client_done = false;
        while (!client_done) {
            int n = ::recv(c, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            inbuf.append(rbuf, rbuf + n);
            std::size_t newline;
            while ((newline = inbuf.find('\n')) != std::string::npos) {
                if (newline > 8192) {
                    static const char err[] = "{\"ok\":false,\"error\":\"line too long\"}\n";
                    socket_send_all(c, err, sizeof(err) - 1);
                    client_done = true;
                    break;
                }
                std::string_view line(inbuf.data(), newline);
                // Strip trailing \r if present.
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                dispatch(o, line, resp);
                resp.push_back('\n');
                if (!socket_send_all(c, resp.data(), resp.size())) {
                    client_done = true;
                    break;
                }
                if (resp.find("\"bye\":true") != std::string::npos) {
                    client_done = true;
                    quit_server = true;
                    break;
                }
                inbuf.erase(0, newline + 1);
            }
            if (!client_done && inbuf.size() > 8192) {
                // Protocol cap from TCP.md.
                static const char err[] = "{\"ok\":false,\"error\":\"line too long\"}\n";
                socket_send_all(c, err, sizeof(err) - 1);
                break;
            }
        }
        CLOSESOCK(c);
    }

    CLOSESOCK(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

struct Args {
    std::string bios = "bios/gba_bios.bin";
    std::string rom;
    std::string bind_address = "127.0.0.1";
    std::string rtc_mode = "fixed";
    uint64_t rtc_epoch_ms = 946684800000ULL;
    int         port = 19843;
    bool no_bios = false;
    bool bios_set = false;
    bool show_help = false;
    bool show_version = false;
};

bool parse_u64_arg(const char* text, uint64_t& value) {
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno || !end || *end) return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_args(int argc, char** argv, Args& a, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--help") { a.show_help = true; continue; }
        if (s == "--version") { a.show_version = true; continue; }
        if (s == "--no-bios") {
            if (a.bios_set) { error = "--bios and --no-bios are mutually exclusive"; return false; }
            a.no_bios = true; a.bios.clear(); continue;
        }
        if (s == "--bios" || s == "--rom" || s == "--bind" ||
            s == "--port" || s == "--rtc-mode" || s == "--rtc-epoch-ms") {
            if (i + 1 >= argc) { error = "missing value for " + std::string(s); return false; }
            const char* value = argv[++i];
            if (!value || !*value) { error = "empty value for " + std::string(s); return false; }
            if (s == "--bios") {
                if (a.no_bios) { error = "--bios and --no-bios are mutually exclusive"; return false; }
                a.bios = value; a.bios_set = true;
            } else if (s == "--rom") a.rom = value;
            else if (s == "--bind") {
                a.bind_address = value;
                if (a.bind_address != "127.0.0.1" && a.bind_address != "0.0.0.0") {
                    error = "--bind must be 127.0.0.1 or 0.0.0.0"; return false;
                }
            } else if (s == "--port") {
                uint64_t port = 0;
                if (!parse_u64_arg(value, port) || !port || port > 65535) {
                    error = "--port must be in 1..65535"; return false;
                }
                a.port = static_cast<int>(port);
            } else if (s == "--rtc-mode") {
                a.rtc_mode = value;
                if (a.rtc_mode != "fixed" && a.rtc_mode != "tick") {
                    error = "--rtc-mode must be fixed or tick"; return false;
                }
            } else {
                if (!parse_u64_arg(value, a.rtc_epoch_ms) ||
                    a.rtc_epoch_ms > 253402300799000ULL) {
                    error = "--rtc-epoch-ms is outside the supported range"; return false;
                }
            }
            continue;
        }
        error = "unknown argument: " + std::string(s);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    mLogSetDefaultLogger(&g_oracle_logger);
    Args args;
    std::string error;
    if (!parse_args(argc, argv, args, error)) {
        std::fprintf(stderr, "oracle: %s\n", error.c_str());
        return 2;
    }
    if (args.show_help) {
        std::puts("gbarecomp_oracle [--rom FILE] [--bios FILE|--no-bios] [--port N] [--bind 127.0.0.1|0.0.0.0] [--rtc-mode fixed|tick] [--rtc-epoch-ms N]");
        return 0;
    }
    if (args.show_version) {
        std::printf("gbarecomp_oracle protocol=2 mGBA=%s commit=%s\n",
                    MGBA_ORACLE_VERSION, MGBA_ORACLE_COMMIT);
        return 0;
    }
    Oracle o;
    if (!o.set_rtc(args.rtc_mode, args.rtc_epoch_ms)) {
        std::fprintf(stderr, "oracle: invalid RTC configuration\n");
        return 2;
    }
    if (!o.init(args.bios.empty() ? nullptr : args.bios.c_str(),
                args.rom.empty() ? nullptr : args.rom.c_str())) {
        return 1;
    }
    int rc = run_server(o, args.port, args.bind_address.c_str());
    o.shutdown();
    return rc;
}
