#pragma once

// More slots on the Load Game screen.
//
// Load Game steps through slots 0..8 with Left and Right: "Slot 1".."Slot 8" read
// savegame.000-.007, and slot 8, "Last Autosave", reads savegame.008. The file name is
// "savegame.00%i", so the slots after it would read savegame.009, savegame.0010, savegame.0011
// and so on. When such files are present in the game folder, the screen's range is extended
// to the highest of them; they show as "Slot 10" onwards. Without them nothing is patched.
// Save Game keeps its own eight slots (a separate counter), so it never writes these.
namespace LoadSlots {

typedef void (*LogFn)(const char* fmt, ...);

// Once the game's code is decrypted (DirectInputCreateA). `gameFolder` ends in a backslash.
void Install(const char* gameFolder, LogFn log);

} // namespace LoadSlots
