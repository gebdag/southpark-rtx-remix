#include "zones.h"

#include <windows.h>
#include <math.h>
#include <string.h>

namespace {

// SouthPark.exe, unpacked in place at its preferred base.
//
//   world +0x1278  number of registered zone sets (grid sections around the camera)
//         +0x127c  per set, 8 bytes: [0] -> resource pointer, [1] -> the set's X/Z rectangle
//   resource R:    table T = R + *(R + 0xc) (sub-table 2, FUN_00442c70); T+4 count, records
//                  from T+8, 0xc4 bytes each
//   record +0x00  float falloff     +0x04 float ambient share +0x08 int subtractive
//          +0x0c u8 flickers        +0x0d u8 shape          +0x0e u8 flicker phase
//          +0x10  float colour[3]   +0x1c float direction[3] (box)
//          +0x28  world -> zone matrix   +0x68 zone -> world matrix (row vectors)
// Inside the zone's own space a sphere is the unit ball, a box the unit cube and a cone the
// 45-degree cone around +x (FUN_00430a30).
//
// The world tick (0x0043d490) registers the sets with FUN_004405e0 and releases them with
// FUN_00440880, which zeroes the count, before the frame is drawn. The release call at
// 0x0043d5db is redirected here, so the zones are copied while they are still registered.
const unsigned int kWorld = 0x00522a68;
const unsigned int kSetCount = kWorld + 0x1278;
const unsigned int kSets = kWorld + 0x127c;
const unsigned int kZoneLightingOn = 0x004d74c8;
const unsigned int kFrameCounter = 0x0053edd0;
const unsigned int kWaveTable = 0x004d86f4;   // 90 signed bytes, also the lit-vertex wave
const int kWaveLength = 90;
const unsigned int kRecordSize = 0xc4;
const int kMaxSets = 128;
const int kMaxRecords = 512;

const unsigned int kReleaseCall = 0x0043d5db;
const unsigned int kRelease = 0x00440880;

// FUN_00441550, which finds the zones around a point, and FUN_00441d90, which turns them into
// an object's light, as built; and the release call itself.
const unsigned char kCollectPrologue[] = {0x83, 0xec, 0x08, 0x53, 0x8b, 0x5c, 0x24, 0x14, 0x55,
                                          0x8b, 0x6c, 0x24, 0x14, 0x8b, 0x03, 0x8b, 0x53, 0x08};
const unsigned char kBlendPrologue[] = {0x83, 0xec, 0x28, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x34, 0x57};
const unsigned char kReleaseCallBytes[] = {0x56, 0xe8, 0xa0, 0x32, 0x00, 0x00};   // push esi; call

typedef void(__cdecl* ReleaseFn)(unsigned int world);

CRITICAL_SECTION g_lock;
std::vector<Zones::Zone> g_latest;

bool Readable(unsigned int address, unsigned int size) {
    return address && !IsBadReadPtr((const void*)address, size);
}

bool Matches(unsigned int address, const unsigned char* bytes, size_t size) {
    return Readable(address, (unsigned int)size) && memcmp((const void*)address, bytes, size) == 0;
}

template <typename T> T Peek(unsigned int address) {
    return *(const T*)address;
}

float Length(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void Normalise(float* v) {
    const float length = Length(v);
    if (length > 1e-6f) {
        v[0] /= length; v[1] /= length; v[2] /= length;
    }
}

// The engine's own flicker: a step through the wave table, offset per zone by its phase,
// added to every colour channel (FUN_00430e40).
float Flicker(unsigned int record) {
    const int phase = Peek<unsigned char>(record + 0xe) % 7;
    const int frame = Peek<int>(kFrameCounter);
    int step = ((phase + 1) * 16 + frame) % kWaveLength;
    if (step < 0) {
        step += kWaveLength;
    }
    return (float)Peek<signed char>(kWaveTable + step);
}

Zones::Zone ReadRecord(unsigned int record) {
    Zones::Zone z;
    z.shape = Peek<unsigned char>(record + 0xd);
    z.subtractive = Peek<int>(record + 0x8) != 0;
    z.flickers = Peek<unsigned char>(record + 0xc) != 0;
    z.ambientShare = Peek<float>(record + 0x4);
    const float flicker = z.flickers ? Flicker(record) : 0.0f;
    for (int c = 0; c < 3; ++c) {
        z.colour[c] = Peek<float>(record + 0x10 + 4 * c) + flicker;
    }

    const float* toWorld = (const float*)(record + 0x68);
    const float w = toWorld[15] != 0.0f ? toWorld[15] : 1.0f;
    for (int c = 0; c < 3; ++c) {
        z.centre[c] = toWorld[12 + c] / w;
    }
    for (int row = 0; row < 3; ++row) {
        const float semiAxis = Length(toWorld + row * 4);
        if (semiAxis > z.radius) {
            z.radius = semiAxis;
        }
    }
    if (z.shape == Zones::Box) {
        // Stored in zone space; directions take the zone -> world matrix without translation.
        const float* d = (const float*)(record + 0x1c);
        for (int c = 0; c < 3; ++c) {
            z.axis[c] = d[0] * toWorld[c] + d[1] * toWorld[4 + c] + d[2] * toWorld[8 + c];
        }
    } else {
        memcpy(z.axis, toWorld, sizeof(z.axis));
    }
    Normalise(z.axis);
    return z;
}

bool SameZone(const Zones::Zone& a, const Zones::Zone& b) {
    return a.shape == b.shape && fabsf(a.radius - b.radius) < 0.5f &&
           fabsf(a.centre[0] - b.centre[0]) < 0.5f && fabsf(a.centre[1] - b.centre[1]) < 0.5f &&
           fabsf(a.centre[2] - b.centre[2]) < 0.5f;
}

void AddOnce(std::vector<Zones::Zone>& zones, const Zones::Zone& z) {
    for (const auto& existing : zones) {
        if (SameZone(existing, z)) {
            return;
        }
    }
    zones.push_back(z);
}

std::vector<Zones::Zone> ReadRegistered() {
    std::vector<Zones::Zone> zones;
    if (!Peek<int>(kZoneLightingOn)) {
        return zones;
    }
    const int sets = Peek<int>(kSetCount);
    if (sets <= 0 || sets > kMaxSets || !Readable(kSets, sets * 8)) {
        return zones;
    }
    for (int s = 0; s < sets; ++s) {
        const unsigned int entry = Peek<unsigned int>(kSets + s * 8);
        if (!Readable(entry, 4)) {
            continue;
        }
        const unsigned int resource = Peek<unsigned int>(entry);
        if (!Readable(resource, 0x10)) {
            continue;
        }
        const unsigned int table = resource + Peek<unsigned int>(resource + 0xc);
        if (!Readable(table, 8)) {
            continue;
        }
        const int count = Peek<int>(table + 4);
        if (count <= 0 || count > kMaxRecords || !Readable(table + 8, count * kRecordSize)) {
            continue;
        }
        for (int i = 0; i < count; ++i) {
            AddOnce(zones, ReadRecord(table + 8 + i * kRecordSize));
        }
    }
    return zones;
}

void __cdecl ReleaseZoneSets(unsigned int world) {
    std::vector<Zones::Zone> zones = ReadRegistered();
    EnterCriticalSection(&g_lock);
    g_latest.swap(zones);
    LeaveCriticalSection(&g_lock);
    ((ReleaseFn)kRelease)(world);
}

// Points the call's rel32 at the replacement.
bool RedirectCall(unsigned int call, void* target) {
    DWORD old;
    if (!VirtualProtect((void*)call, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    *(int*)(call + 1) = (int)((unsigned int)target - (call + 5));
    VirtualProtect((void*)call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)call, 5);
    return true;
}

} // namespace

bool Zones::Install() {
    InitializeCriticalSection(&g_lock);
    if (!Matches(0x00441550, kCollectPrologue, sizeof(kCollectPrologue)) ||
        !Matches(0x00441d90, kBlendPrologue, sizeof(kBlendPrologue)) ||
        !Matches(kReleaseCall - 1, kReleaseCallBytes, sizeof(kReleaseCallBytes))) {
        return false;
    }
    // The game calls into this module from now on, so it must outlive any FreeLibrary.
    HMODULE self;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (const char*)&ReleaseZoneSets, &self)) {
        return false;
    }
    return RedirectCall(kReleaseCall, (void*)&ReleaseZoneSets);
}

std::vector<Zones::Zone> Zones::Latest() {
    EnterCriticalSection(&g_lock);
    std::vector<Zones::Zone> zones = g_latest;
    LeaveCriticalSection(&g_lock);
    return zones;
}
