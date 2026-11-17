#include "original.h"

#include "config.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

namespace {

HMODULE g_original = 0;
char g_storePath[MAX_PATH];

void RefPath(const char* file, char* out, size_t size) {
    _snprintf(out, size, "%slookdev\\ref\\%s", Config::BaseDir(), file);
    out[size - 1] = 0;
}

// Loads a DLL from lookdev\ref, resolving its own dependencies from that folder first.
HMODULE LoadFromRef(const char* file) {
    char path[MAX_PATH];
    RefPath(file, path, sizeof(path));
    HMODULE m = LoadLibraryExA(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    LogLine("    lookdev: %s %s", m ? "loaded" : "FAILED to load", path);
    return m;
}

// Calls `redirect(name)` for every import `module` takes from `dll`, and writes what it returns
// into that import's slot; a null return leaves the slot alone. Returns how many slots were
// rewritten, or -1 for an import by ordinal, which has no name to go by.
template <class Redirect>
int RedirectImports(HMODULE module, const char* dll, Redirect redirect) {
    unsigned char* base = (unsigned char*)module;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int rewritten = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR* d =
             (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         d->Name; ++d) {
        if (lstrcmpiA((const char*)(base + d->Name), dll) != 0) {
            continue;
        }
        const IMAGE_THUNK_DATA* names =
            (const IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                    : d->FirstThunk));
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                LogLine("    lookdev: the original imports from %s by ordinal -- cannot redirect", dll);
                return -1;
            }
            const char* name =
                (const char*)((const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            const FARPROC to = redirect(name);
            if (!to) {
                continue;
            }
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old);
            slots->u1.Function = (ULONG_PTR)to;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            ++rewritten;
        }
    }
    return rewritten;
}

// The original confines the cursor to its window unless a "develop.er" file sits in the
// working directory -- the game folder. Side by side that would keep the mouse from the Remix
// window, so the confinement is declined here instead of adding a file to the game folder.
BOOL WINAPI KeepCursorFree(const RECT*) {
    return TRUE;
}

} // namespace

bool Original::Load() {
    if (g_original) {
        return true;
    }
    // dgVoodoo2 reads its settings from the application's folder -- the game folder -- and
    // without them it takes the original into exclusive fullscreen and changes the display mode.
    char conf[MAX_PATH];
    _snprintf(conf, sizeof(conf), "%sdgVoodoo.conf", Config::BaseDir());
    conf[sizeof(conf) - 1] = 0;
    if (GetFileAttributesA(conf) == INVALID_FILE_ATTRIBUTES) {
        LogLine("    lookdev: no %s -- not starting the original (it would go fullscreen)", conf);
        return false;
    }
    // dgVoodoo's DDraw.dll finds its D3DImm.dll beside it; loading that first keeps any later
    // load of it by name on this copy too.
    if (!LoadFromRef("D3DImm.dll")) {
        return false;
    }
    HMODULE wrapper = LoadFromRef("DDraw.dll");
    if (!wrapper) {
        return false;
    }
    HMODULE original = LoadFromRef("d3d_original.dll");
    if (!original) {
        return false;
    }
    // Every DirectDraw import goes to dgVoodoo2; one it lacks fails the whole redirect.
    bool complete = true;
    const int rebound = RedirectImports(original, "DDRAW.dll", [&](const char* name) {
        const FARPROC to = GetProcAddress(wrapper, name);
        if (!to) {
            LogLine("    lookdev: dgVoodoo2's DDraw.dll has no %s", name);
            complete = false;
        }
        return to;
    });
    if (rebound <= 0 || !complete) {
        LogLine("    lookdev: could not point the original's DirectDraw imports at dgVoodoo2");
        FreeLibrary(original);
        return false;
    }
    RedirectImports(original, "USER32.dll", [](const char* name) {
        return lstrcmpA(name, "ClipCursor") == 0 ? (FARPROC)&KeepCursorFree : (FARPROC)0;
    });
    RefPath("d3d\\win32.tex", g_storePath, sizeof(g_storePath));
    char dir[MAX_PATH];
    RefPath("d3d", dir, sizeof(dir));
    CreateDirectoryA(dir, 0);
    g_original = original;
    LogLine("    lookdev: the original renderer draws the game window (%d DirectDraw imports "
            "on dgVoodoo2)", rebound);
    return true;
}

bool Original::Active() {
    return g_original != 0;
}

FARPROC Original::Export(const char* name) {
    return g_original ? GetProcAddress(g_original, name) : 0;
}

const char* Original::TextureStorePath() {
    return g_storePath;
}
