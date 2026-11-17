#pragma once

#include <windows.h>
#include <stdio.h>
#include <string.h>

// Which build of SouthPark.exe is running, and where the US build's addresses are in it.
//
// Every game address in the renderer, the Remix proxy and the input shim is the US build's
// (disc "South Park (USA) (En,Fr,De,Es,Pt)", SouthPark.exe md5 126c2e2c...). Other builds --
// the European and German retail discs, and any other -- are the same game laid out
// differently. Code that touches the game passes US addresses through Place().
//
// On the US build Place() returns its input. On any other build the game's code is searched,
// once, for the signatures in game_build_signatures.h: code bytes with their addresses left
// open, each kept only if it finds the right answer in the US, European and German builds.
// What one finds is that build's address; an address without a signature, or
// whose signature is not found exactly once, moves with the nearest found one, and the byte
// checks in front of every patch still decide whether it is applied.
//
// The build is told by the timestamp in SouthPark.exe's header, which the copy protection
// leaves as it is. The search needs the decrypted code, so nothing here may run before the
// game's own code has started; the modules first call it when they patch the game.
namespace GameBuild {

struct Pair {
    unsigned int us;
    unsigned int here;
};

// CODE: the address is `at` bytes into the match. OPERAND: the dword `at` bytes in is the
// address. BRANCH: the rel32 `at` bytes in leads to it.
enum Kind { Code, Operand, Branch };

struct Signature {
    unsigned int us;
    Kind kind;
    int at;
    int length;
    const char* bytes;
    const char* mask;   // 'x' must match, '?' is open
};

#include "game_build_signatures.h"

const unsigned int kUsStamp = 0x36af2fca;
const unsigned int kImageLow = 0x00401000;
const unsigned int kImageHigh = 0x01600000;
// An address within this of a found one, either side -- a call site's operand, the byte in
// front of an instruction, a field of a global -- moves with it.
const unsigned int kNear = 0x100;
const int kMaxPairs = sizeof(kSignatures) / sizeof(kSignatures[0]);

struct Table {
    bool us;
    char name[96];
    Pair pairs[kMaxPairs];
    int count;
};

inline const IMAGE_NT_HEADERS* ExeHeaders() {
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(nullptr);
    return (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
}

// Where `sig` matches exactly once in [text, end), or nullptr.
inline const unsigned char* FindOnce(const unsigned char* text, const unsigned char* end, const Signature& sig) {
    int fixed = 0;
    while (fixed < sig.length && sig.mask[fixed] != 'x') {
        ++fixed;
    }
    if (fixed == sig.length) {
        return nullptr;
    }
    const unsigned char first = (unsigned char)sig.bytes[fixed];
    const unsigned char* found = nullptr;
    const unsigned char* p = text + fixed;
    while (p < end) {
        p = (const unsigned char*)memchr(p, first, end - p);
        if (!p) {
            break;
        }
        const unsigned char* start = p - fixed;
        ++p;
        if (start + sig.length > end) {
            break;
        }
        int i = 0;
        while (i < sig.length && (sig.mask[i] != 'x' || start[i] == (unsigned char)sig.bytes[i])) {
            ++i;
        }
        if (i == sig.length) {
            if (found) {
                return nullptr;
            }
            found = start;
        }
    }
    return found;
}

inline unsigned int Resolve(const unsigned char* match, const Signature& sig) {
    const unsigned char* at = match + sig.at;
    unsigned int dword;
    memcpy(&dword, at, 4);
    switch (sig.kind) {
    case Code: return (unsigned int)at;
    case Operand: return dword;
    default: return (unsigned int)at + 4 + dword;
    }
}

// The signatures for one US address are next to each other; the most common answer wins.
inline void Search(Table& t) {
    const IMAGE_NT_HEADERS* nt = ExeHeaders();
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(nullptr);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    const unsigned char* text = nullptr;
    const unsigned char* end = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            text = base + section->VirtualAddress;
            end = text + section->Misc.VirtualSize;
            break;
        }
    }
    if (!text || IsBadReadPtr(text, end - text)) {
        return;
    }
    unsigned int answers[8];
    int votes[8];
    for (int s = 0; s < kMaxPairs;) {
        const unsigned int us = kSignatures[s].us;
        int distinct = 0;
        for (; s < kMaxPairs && kSignatures[s].us == us; ++s) {
            const unsigned char* match = FindOnce(text, end, kSignatures[s]);
            if (!match) {
                continue;
            }
            const unsigned int answer = Resolve(match, kSignatures[s]);
            int k = 0;
            while (k < distinct && answers[k] != answer) {
                ++k;
            }
            if (k == distinct && distinct < 8) {
                answers[distinct] = answer;
                votes[distinct++] = 0;
            }
            if (k < distinct) {
                ++votes[k];
            }
        }
        int best = -1;
        for (int k = 0; k < distinct; ++k) {
            if (best < 0 || votes[k] > votes[best]) {
                best = k;
            }
        }
        if (best >= 0) {
            t.pairs[t.count++] = {us, answers[best]};
        }
    }
}

inline const char* KnownName(unsigned int stamp) {
    switch (stamp) {
    case 0x36c3065d: return "Europe (En,Fr,Es)";
    case 0x36c303e2: return "Germany";
    default: return "unknown";
    }
}

inline Table Searched(unsigned int stamp) {
    Table t = {};
    Search(t);
    int addresses = 0;
    for (int s = 0; s < kMaxPairs; ++s) {
        addresses += (s == 0 || kSignatures[s].us != kSignatures[s - 1].us) ? 1 : 0;
    }
    _snprintf(t.name, sizeof(t.name), "%s (stamp 0x%08x): %d of %d addresses found", KnownName(stamp), stamp,
              t.count, addresses);
    t.name[sizeof(t.name) - 1] = 0;
    return t;
}

// The renderer, the Remix proxy and the input shim each patch the game, and a signature that
// covers bytes another one has patched no longer matches. So the game is searched once, by
// whichever module asks first -- each asks before it patches anything -- and the others take
// that table from memory shared within this process.
struct Shared {
    volatile LONG ready;
    int signatures;
    Table table;
};

inline Table SearchedOnce(unsigned int stamp) {
    char name[64];
    _snprintf(name, sizeof(name), "Local\\SouthParkGameBuild.%lu", GetCurrentProcessId());
    name[sizeof(name) - 1] = 0;
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
    const bool first = GetLastError() != ERROR_ALREADY_EXISTS;
    Shared* shared = mapping ? (Shared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)) : nullptr;
    if (!shared) {
        return Searched(stamp);
    }
    // The mapping stays open for the life of the process, so a module loaded later finds it.
    if (first) {
        shared->table = Searched(stamp);
        shared->signatures = kMaxPairs;
        InterlockedExchange(&shared->ready, 1);
        return shared->table;
    }
    for (int wait = 0; !shared->ready && wait < 2000; ++wait) {
        Sleep(1);
    }
    // A module built with other signatures searches for itself.
    return shared->ready && shared->signatures == kMaxPairs ? shared->table : Searched(stamp);
}

inline const Table& Running() {
    static Table table = [] {
        const unsigned int stamp = ExeHeaders()->FileHeader.TimeDateStamp;
        if (stamp == kUsStamp) {
            Table t = {};
            t.us = true;
            lstrcpynA(t.name, "USA (En,Fr,De,Es,Pt)", sizeof(t.name));
            return t;
        }
        return SearchedOnce(stamp);
    }();
    return table;
}

inline const char* Name() {
    return Running().name;
}

// The running build's address for US address `us`, or 0 when nothing found is near it. Values
// outside the executable's image are returned as they are: they are not addresses.
inline unsigned int Place(unsigned int us) {
    const Table& t = Running();
    if (t.us || us < kImageLow || us >= kImageHigh) {
        return us;
    }
    int lo = 0, hi = t.count;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (t.pairs[mid].us <= us) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // pairs[lo - 1] is the nearest found address at or below `us`, pairs[lo] the nearest above.
    const unsigned int below = lo > 0 ? us - t.pairs[lo - 1].us : kNear;
    const unsigned int above = lo < t.count ? t.pairs[lo].us - us : kNear;
    if (below >= kNear && above >= kNear) {
        return 0;
    }
    const Pair& p = below <= above ? t.pairs[lo - 1] : t.pairs[lo];
    return us + (p.here - p.us);
}

template <typename T> T* Place(T* us) {
    return (T*)Place((unsigned int)us);
}

// Rewrites the US addresses embedded in code bytes (an operand inside an expected or relocated
// instruction) as the running build's. Only addresses that were found are rewritten.
inline void PlaceEmbedded(unsigned char* bytes, size_t size) {
    const Table& t = Running();
    if (t.us) {
        return;
    }
    for (size_t i = 0; i + 4 <= size; ++i) {
        unsigned int value;
        memcpy(&value, bytes + i, 4);
        for (int k = 0; k < t.count; ++k) {
            if (t.pairs[k].us == value) {
                memcpy(bytes + i, &t.pairs[k].here, 4);
                i += 3;
                break;
            }
        }
    }
}

} // namespace GameBuild
