#pragma once
#include <string>
#include <vector>
// Sound output (miniaudio). Files load through the VFS: .opus (Ogg Opus, what
// assetc produces and the game ships) plus .wav/.flac/.mp3 for convenience.
// Everything streams, so long music costs no more memory than a short sound.
// World space matches the renderer: Z up, game units.
#include <glm/glm.hpp>

#include <optional>
#include <string_view>

namespace keel::audio {

// noDevice: mix without an output device (tests, headless); read with readFrames.
bool init(bool noDevice = false);
void shutdown();
bool active();

// Call once per frame: places the listener and frees finished sounds.
void update(const glm::vec3& listener, float yaw, float pitch);
// Silences everything while the window is in the background (snd_mute_losefocus).
void setFocused(bool focused);

// One-shot sound; with a position it's 3D, otherwise it plays flat (UI, local).
// Hot reload: sounds whose files changed are read again on their next play. Returns their paths.
std::vector<std::string> reloadChanged();
void play(std::string_view path, std::optional<glm::vec3> position = std::nullopt, float volume = 1.0f);
// fadeMs: rises from silence over that long.
void playMusic(std::string_view path, bool loop = true, int fadeMs = 0);
void stopMusic();
// A looping background sound (rain, wind) at a volume, set every frame or when it changes;
// 0 stops it. It eases to the new volume rather than jumping.
void setAmbience(std::string_view path, float volume);
// A looping sound at a place (a car's engine), by key: set it every frame, with its volume and
// pitch (1 as recorded); volume 0 stops it. Keys not set for a second stop by themselves.
void setLoop(int key, std::string_view path, const glm::vec3& position, float volume, float pitch);
// Stops every sound playing (not the music): an editor's Stop.
void stopSounds();
int playingCount();

// Headless mixing (init(true)): stereo float frames at 48 kHz.
void readFrames(float* out, int frames);

} // namespace keel::audio
