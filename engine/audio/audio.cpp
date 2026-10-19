#include "audio/audio.h"
#include "core/cvar.h"
#include "core/log.h"
#include "fs/vfs.h"

#include <miniaudio.h>
#include <opusfile.h>

#include <algorithm>
#include <random>
#include <cmath>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace keel::audio {
namespace {
Cvar s_volume("volume", "0.8", CVAR_ARCHIVE, "Master volume 0..1");
Cvar s_musicvolume("s_musicvolume", "0.3", CVAR_ARCHIVE, "Music volume 0..1");

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

// ── Ogg Opus as a miniaudio data source, decoding from memory as it plays ──
struct OpusSource {
    ma_data_source_base base;
    Bytes bytes;
    OggOpusFile* file = nullptr;
    int channels = 1;
};

ma_result opusRead(ma_data_source* ds, void* out, ma_uint64 frames, ma_uint64* read) {
    auto* s = static_cast<OpusSource*>(ds);
    ma_uint64 done = 0;
    float* dst = static_cast<float*>(out);
    while (done < frames) {
        const int n = s->channels == 2 ? op_read_float_stereo(s->file, dst + done * 2, int((frames - done) * 2))
                                       : op_read_float(s->file, dst + done, int(frames - done), nullptr);
        if (n <= 0) break;
        done += ma_uint64(n);
    }
    *read = done;
    return done == 0 && frames > 0 ? MA_AT_END : MA_SUCCESS;
}
ma_result opusSeek(ma_data_source* ds, ma_uint64 frame) {
    return op_pcm_seek(static_cast<OpusSource*>(ds)->file, ogg_int64_t(frame)) == 0 ? MA_SUCCESS : MA_ERROR;
}
ma_result opusFormat(ma_data_source* ds, ma_format* fmt, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t mapCap) {
    *fmt = ma_format_f32;
    *channels = ma_uint32(static_cast<OpusSource*>(ds)->channels);
    *rate = 48000; // Opus always decodes at 48 kHz
    if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, *channels);
    return MA_SUCCESS;
}
ma_result opusCursor(ma_data_source* ds, ma_uint64* cursor) {
    const ogg_int64_t t = op_pcm_tell(static_cast<OpusSource*>(ds)->file);
    *cursor = t < 0 ? 0 : ma_uint64(t);
    return MA_SUCCESS;
}
ma_result opusLength(ma_data_source* ds, ma_uint64* length) {
    const ogg_int64_t t = op_pcm_total(static_cast<OpusSource*>(ds)->file, -1);
    *length = t < 0 ? 0 : ma_uint64(t);
    return MA_SUCCESS;
}
const ma_data_source_vtable kOpusVtable = {opusRead, opusSeek, opusFormat, opusCursor, opusLength, nullptr, 0};

// A playing sound and whatever decodes it.
struct Voice {
    ma_sound sound{};
    OpusSource opus{};
    ma_decoder decoder{};
    bool isOpus = false, hasDecoder = false, music = false;
    Bytes bytes;
    ~Voice() {
        ma_sound_uninit(&sound);
        if (isOpus) {
            op_free(opus.file);
            ma_data_source_uninit(&opus.base);
        }
        if (hasDecoder) ma_decoder_uninit(&decoder);
    }
};

std::unique_ptr<ma_engine> g_engine;
std::list<Voice> g_voices;
std::unordered_map<std::string, Bytes> g_cache; // file bytes, shared by voices
std::unordered_map<std::string, int64_t> g_times; // their files' times (hot reload)
Voice* g_music = nullptr;
std::unordered_map<std::string, Voice*> g_ambience; // looping background sounds by path
struct Loop {
    Voice* voice;
    std::string path;
    int frame; // the update it was last set in
};
std::unordered_map<int, Loop> g_loops; // looping sounds in the world (setLoop)
int g_frame = 0;

Bytes load(std::string_view path) {
    const std::string key(path);
    if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
    auto data = fs::read(path);
    if (!data) log::warn("Sound not found: {}", key);
    if (auto t = fs::mtime(key)) g_times[key] = *t;
    return g_cache[key] = data ? std::make_shared<const std::vector<uint8_t>>(std::move(*data)) : nullptr;
}

Voice* start(std::string_view path, bool spatial) {
    Bytes bytes = load(path);
    if (!g_engine || !bytes) return nullptr;
    Voice& v = g_voices.emplace_back();
    v.bytes = bytes;
    ma_data_source* source = nullptr;
    if (path.ends_with(".opus") || path.ends_with(".ogg")) {
        int err = 0;
        v.opus.file = op_open_memory(bytes->data(), bytes->size(), &err);
        if (!v.opus.file) {
            log::warn("{}: not an Ogg Opus file ({})", path, err);
            g_voices.pop_back();
            return nullptr;
        }
        v.isOpus = true;
        v.opus.channels = std::min(op_channel_count(v.opus.file, -1), 2);
        ma_data_source_config cfg = ma_data_source_config_init();
        cfg.vtable = &kOpusVtable;
        ma_data_source_init(&cfg, &v.opus.base);
        source = &v.opus.base;
    } else {
        if (ma_decoder_init_memory(bytes->data(), bytes->size(), nullptr, &v.decoder) != MA_SUCCESS) {
            log::warn("{}: unsupported audio file", path);
            g_voices.pop_back();
            return nullptr;
        }
        v.hasDecoder = true;
        source = &v.decoder;
    }
    const ma_uint32 flags = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(g_engine.get(), source, flags, nullptr, &v.sound) != MA_SUCCESS) {
        g_voices.pop_back();
        return nullptr;
    }
    if (spatial) {
        ma_sound_set_attenuation_model(&v.sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&v.sound, 80.0f);
        ma_sound_set_max_distance(&v.sound, 3000.0f);
    }
    return &v;
}

Cmd cmd_play("play", [](const Args& a) {
    if (a.size() > 1) play(a[1]);
}, "Play a sound file");
} // namespace

bool init(bool noDevice) {
    g_engine = std::make_unique<ma_engine>();
    ma_engine_config cfg = ma_engine_config_init();
    if (noDevice) {
        cfg.noDevice = MA_TRUE;
        cfg.channels = 2;
        cfg.sampleRate = 48000;
    }
    if (ma_engine_init(&cfg, g_engine.get()) != MA_SUCCESS) {
        log::warn("No audio device; sound is off");
        g_engine.reset();
        return false;
    }
    ma_engine_listener_set_world_up(g_engine.get(), 0, 0, 0, 1);
    return true;
}

void shutdown() {
    g_music = nullptr;
    g_ambience.clear();
    g_loops.clear();
    g_voices.clear();
    g_cache.clear();
    if (g_engine) ma_engine_uninit(g_engine.get());
    g_engine.reset();
}

bool active() { return g_engine != nullptr; }

Cvar snd_mute_losefocus("snd_mute_losefocus", "1", CVAR_ARCHIVE, "Silence it while its window isn't focused");
bool g_unfocused = false;
float masterVolume() { return g_unfocused && snd_mute_losefocus.b() ? 0.0f : std::clamp(s_volume.f(), 0.0f, 1.0f); }

void setFocused(bool focused) {
    g_unfocused = !focused;
    if (g_engine) ma_engine_set_volume(g_engine.get(), masterVolume());
}

void update(const glm::vec3& listener, float yaw, float pitch) {
    if (!g_engine) return;
    ma_engine_set_volume(g_engine.get(), masterVolume());
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    ma_engine_listener_set_position(g_engine.get(), 0, listener.x, listener.y, listener.z);
    ma_engine_listener_set_direction(g_engine.get(), 0, std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
    if (g_music) ma_sound_set_volume(&g_music->sound, std::clamp(s_musicvolume.f(), 0.0f, 1.0f));
    g_voices.remove_if([](Voice& v) { return !v.music && ma_sound_at_end(&v.sound); });
    // Loops nobody set lately (their car went away) stop.
    ++g_frame;
    for (auto it = g_loops.begin(); it != g_loops.end();) {
        if (g_frame - it->second.frame > 60) {
            Voice* v = it->second.voice;
            g_voices.remove_if([&](Voice& x) { return &x == v; });
            it = g_loops.erase(it);
        } else ++it;
    }
}

void setLoop(int key, std::string_view path, const glm::vec3& position, float volume, float pitch) {
    auto it = g_loops.find(key);
    if (it != g_loops.end() && (volume <= 0.001f || it->second.path != path)) {
        Voice* v = it->second.voice;
        g_voices.remove_if([&](Voice& x) { return &x == v; });
        g_loops.erase(it);
        it = g_loops.end();
    }
    if (volume <= 0.001f) return;
    if (it == g_loops.end()) {
        Voice* v = start(path, true);
        if (!v) return;
        v->music = true; // (kept when it ends; it loops anyway)
        ma_sound_set_looping(&v->sound, MA_TRUE);
        ma_sound_start(&v->sound);
        it = g_loops.emplace(key, Loop{v, std::string(path), g_frame}).first;
    }
    Loop& l = it->second;
    l.frame = g_frame;
    ma_sound_set_position(&l.voice->sound, position.x, position.y, position.z);
    ma_sound_set_volume(&l.voice->sound, volume);
    ma_sound_set_pitch(&l.voice->sound, std::clamp(pitch, 0.1f, 4.0f));
}

// Variants: "foo.opus" missing but "foo1.opus", "foo2.opus"... there → one of those at random.
std::string pickVariant(std::string_view path) {
    static std::unordered_map<std::string, int> counts; // 0 = use the path as is
    static std::mt19937 rng{std::random_device{}()};
    const std::string key(path);
    auto it = counts.find(key);
    if (it == counts.end()) {
        int n = 0;
        if (!fs::exists(key)) {
            const size_t dot = key.rfind('.');
            const std::string stem = key.substr(0, dot), ext = dot == std::string::npos ? "" : key.substr(dot);
            while (fs::exists(stem + std::to_string(n + 1) + ext)) ++n;
        }
        it = counts.emplace(key, n).first;
    }
    if (it->second == 0) return key;
    const size_t dot = key.rfind('.');
    return key.substr(0, dot) + std::to_string(std::uniform_int_distribution<int>(1, it->second)(rng)) +
           (dot == std::string::npos ? "" : key.substr(dot));
}

std::vector<std::string> reloadChanged() {
    std::vector<std::string> changed;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        const auto t = fs::mtime(it->first);
        const auto known = g_times.find(it->first);
        if (t && known != g_times.end() && known->second != *t) {
            changed.push_back(it->first);
            g_times.erase(known);
            it = g_cache.erase(it); // playing voices keep their bytes; the next play reads the new file
        } else {
            ++it;
        }
    }
    return changed;
}

void play(std::string_view path, std::optional<glm::vec3> position, float volume) {
    if (g_voices.size() > 128) return; // something is spamming; drop rather than stutter
    Voice* v = start(pickVariant(path), position.has_value());
    if (!v) return;
    if (position) ma_sound_set_position(&v->sound, position->x, position->y, position->z);
    ma_sound_set_volume(&v->sound, volume);
    // A slightly different pitch each time, so repeated sounds don't sound copy-pasted.
    static std::mt19937 jitter{12345};
    ma_sound_set_pitch(&v->sound, std::uniform_real_distribution<float>(0.95f, 1.05f)(jitter));
    ma_sound_start(&v->sound);
}

void playMusic(std::string_view path, bool loop, int fadeMs) {
    stopMusic();
    g_music = start(path, false);
    if (!g_music) return;
    g_music->music = true;
    ma_sound_set_looping(&g_music->sound, loop);
    ma_sound_set_volume(&g_music->sound, std::clamp(s_musicvolume.f(), 0.0f, 1.0f));
    if (fadeMs > 0) ma_sound_set_fade_in_milliseconds(&g_music->sound, 0.0f, 1.0f, ma_uint64(fadeMs));
    ma_sound_start(&g_music->sound);
}

void stopMusic() {
    if (!g_music) return;
    g_voices.remove_if([](Voice& v) { return &v == g_music; });
    g_music = nullptr;
}

void setAmbience(std::string_view path, float volume) {
    const std::string key(path);
    auto it = g_ambience.find(key);
    if (volume <= 0.001f) {
        if (it != g_ambience.end()) {
            Voice* v = it->second;
            g_voices.remove_if([&](Voice& x) { return &x == v; });
            g_ambience.erase(it);
        }
        return;
    }
    if (it == g_ambience.end()) {
        Voice* v = start(path, false);
        if (!v) return;
        v->music = true; // (kept when it ends; it loops anyway)
        ma_sound_set_looping(&v->sound, MA_TRUE);
        ma_sound_set_volume(&v->sound, 0.0f);
        ma_sound_start(&v->sound);
        it = g_ambience.emplace(key, v).first;
    }
    const float now = ma_sound_get_volume(&it->second->sound);
    ma_sound_set_volume(&it->second->sound, now + (volume - now) * 0.08f);
}

int playingCount() { return int(g_voices.size()); }

void stopSounds() { g_voices.remove_if([](Voice& v) { return !v.music; }); }

void readFrames(float* out, int frames) {
    if (g_engine) ma_engine_read_pcm_frames(g_engine.get(), out, ma_uint64(frames), nullptr);
}

} // namespace keel::audio
