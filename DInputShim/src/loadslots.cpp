#include "loadslots.h"

#include <windows.h>
#include <stdio.h>

#include "game_patch.h"

namespace {

const int kAutosaveSlot = 8;
// The range is patched into imm8 operands, which the CPU sign-extends.
const int kHighestSlot = 127;

// Load Game's Left/Right handling (US build). The slot is [0x105b5a4].
//   48cec0  mov eax,[slot]; add eax,esi; cmp eax,8      -- past the last slot wraps to 0
//   48cede  mov dword [slot],8                          -- before the first wraps to the last,
//                                                          then falls into the autosave label
//   48cf40  inc eax; push eax; lea eax,[esp+14]; push   -- the "Slot %i" label
const unsigned int kStep = 0x0048cec0;
const unsigned char kStepBytes[] = {0xa1, 0xa4, 0xb5, 0x05, 0x01, 0x03, 0xc6, 0x83, 0xf8, kAutosaveSlot};
const unsigned int kStepLast = kStep + 9;   // the 8 of cmp eax,8

const unsigned int kWrapBack = 0x0048cede;
const unsigned char kWrapBackBytes[] = {0xc7, 0x05, 0xa4, 0xb5, 0x05, 0x01, kAutosaveSlot, 0x00, 0x00, 0x00};

const unsigned int kSlotLabel = 0x0048cf40;
const unsigned char kSlotLabelBytes[] = {0x40, 0x50, 0x8d, 0x44, 0x24, 0x14, 0x68};

// The highest slot past the autosave with a file behind it, or the autosave slot if none.
int HighestSlot(const char* gameFolder) {
    int highest = kAutosaveSlot;
    for (int slot = kAutosaveSlot + 1; slot <= kHighestSlot; ++slot) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%ssavegame.00%i", gameFolder, slot);
        path[MAX_PATH - 1] = 0;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            highest = slot;
        }
    }
    return highest;
}

} // namespace

void LoadSlots::Install(const char* gameFolder, LogFn log) {
    const int highest = HighestSlot(gameFolder);
    if (highest == kAutosaveSlot) {
        return;
    }
    if (!GamePatch::Matches(kStep, kStepBytes, sizeof(kStepBytes)) ||
        !GamePatch::Matches(kWrapBack, kWrapBackBytes, sizeof(kWrapBackBytes)) ||
        !GamePatch::Matches(kSlotLabel, kSlotLabelBytes, sizeof(kSlotLabelBytes))) {
        log("load slots: Load Game's slot code is not where expected -- left at 9 slots");
        return;
    }
    // push highest; pop eax; mov [slot],eax; jmp short to the "Slot %i" label.
    const int jump = (int)GameBuild::Place(kSlotLabel) - (int)(GameBuild::Place(kWrapBack) + 10);
    if (jump < 0 || jump > 127) {
        log("load slots: the slot label is out of a short jump's reach -- left at 9 slots");
        return;
    }
    const unsigned char wrapBack[] = {0x6a, (unsigned char)highest, 0x58, 0xa3, 0xa4, 0xb5, 0x05, 0x01,
                                      0xeb, (unsigned char)jump};
    GamePatch::Write<unsigned char>(kStepLast, (unsigned char)highest);
    GamePatch::WriteBytes(kWrapBack, wrapBack, sizeof(wrapBack));
    log("load slots: Load Game offers slots up to %d (savegame.00%d)", highest + 1, highest);
}
