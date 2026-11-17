#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

namespace {
char g_path[MAX_PATH];
bool g_open = false;
}

void LogOpen(const char* directory) {
    _snprintf(g_path, sizeof(g_path), "%sRemixProxy.log", directory);
    g_path[sizeof(g_path) - 1] = 0;
    DeleteFileA(g_path);
    g_open = true;
}

// All callers are on the game's thread: the renderer, the window procedure and the bridge's
// callbacks all run there.
void Log(const char* fmt, ...) {
    if (!g_open) {
        return;
    }
    FILE* f = fopen(g_path, "a");
    if (!f) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}
