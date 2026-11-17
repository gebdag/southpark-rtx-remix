#pragma once

#include <windows.h>
#include <string.h>

#include "game_build.h"

// Reading and patching the game's code and data. Every address handed in is the US build's
// (game_build.h) and is placed in the running build here, so callers only ever use US
// addresses. An expected dword that is itself an address (a jump-table entry), and addresses
// embedded in expected byte strings, are placed too.
namespace GamePatch {

template <typename T> bool Is(unsigned int address, T expected) {
    address = GameBuild::Place(address);
    if (sizeof(T) == 4) {
        expected = (T)GameBuild::Place((unsigned int)expected);
    }
    return !IsBadReadPtr((const void*)address, sizeof(T)) && *(const T*)address == expected;
}

inline bool Matches(unsigned int address, const unsigned char* bytes, size_t size) {
    unsigned char placed[16];
    if (size > sizeof(placed)) {
        return false;
    }
    memcpy(placed, bytes, size);
    GameBuild::PlaceEmbedded(placed, size);
    address = GameBuild::Place(address);
    return !IsBadReadPtr((const void*)address, size) && memcmp((const void*)address, placed, size) == 0;
}

template <typename T> void Write(unsigned int address, T value) {
    address = GameBuild::Place(address);
    DWORD old;
    VirtualProtect((void*)address, sizeof(T), PAGE_EXECUTE_READWRITE, &old);
    *(T*)address = value;
    VirtualProtect((void*)address, sizeof(T), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, sizeof(T));
}

// Writes `size` bytes, with any addresses embedded in them placed.
inline void WriteBytes(unsigned int address, const unsigned char* bytes, size_t size) {
    unsigned char placed[16];
    if (size > sizeof(placed)) {
        return;
    }
    memcpy(placed, bytes, size);
    GameBuild::PlaceEmbedded(placed, size);
    address = GameBuild::Place(address);
    DWORD old;
    VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)address, placed, size);
    VirtualProtect((void*)address, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
}

} // namespace GamePatch
