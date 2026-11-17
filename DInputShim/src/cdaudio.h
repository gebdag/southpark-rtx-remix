#pragma once

// South Park's soundtrack is the disc's CD audio, tracks 2 to 14, which the game plays through
// MCI's cdaudio device. When the game folder holds a `music` folder with the tracks as WAV files
// (track02.wav ... track14.wav, numbered as on the disc), the cdaudio device the game opens is
// answered here instead and plays those files, so the music does not depend on a drive that
// can play audio. Without the folder nothing changes.
namespace CdAudio {

typedef void (*LogFn)(const char* format, ...);

// From DllMain, with the folder that holds the game. Hooks winmm's mciSendCommandA, which the
// executable imports statically, so it is resident; nothing here loads a module.
void Install(const char* gameFolder, LogFn log);

} // namespace CdAudio
