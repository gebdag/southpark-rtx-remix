#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace {

FILE* g_file = 0;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

bool g_callLogging = false;
unsigned int g_callIndex = 0;
const char* g_lastName = 0;
unsigned int g_repeatCount = 0;

void WriteRaw(const char* text) {
    if (g_file) {
        fputs(text, g_file);
        fflush(g_file);
    }
    OutputDebugStringA(text);
}

// A run of identical consecutive export names is emitted as a single "xN" line
// once the run ends, so a 60fps frame loop does not bury the interesting calls.
void FlushRepeat() {
    if (g_repeatCount > 1) {
        char buf[256];
        _snprintf(buf, sizeof(buf), "      ... %s x%u\n", g_lastName, g_repeatCount);
        buf[sizeof(buf) - 1] = 0;
        WriteRaw(buf);
    }
    g_repeatCount = 0;
}

} // namespace

void LogOpen(const char* path) {
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (!g_file) {
        g_file = fopen(path, "w");
    }
    LogLine("=== Video_D3D9 loaded ===");
}

void LogClose() {
    if (!g_lockReady) {
        return;
    }
    EnterCriticalSection(&g_lock);
    FlushRepeat();
    if (g_file) {
        fclose(g_file);
        g_file = 0;
    }
    LeaveCriticalSection(&g_lock);
}

void LogLine(const char* fmt, ...) {
    if (!g_lockReady) {
        return;
    }
    EnterCriticalSection(&g_lock);
    FlushRepeat();
    g_lastName = 0;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 2] = 0;

    size_t len = strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = 0;
    WriteRaw(buf);

    LeaveCriticalSection(&g_lock);
}

void LogSetCallLogging(bool enabled) {
    g_callLogging = enabled;
}

void LogCall(const char* name, const char* fmt, ...) {
    if (!g_lockReady || !g_callLogging) {
        return;
    }
    EnterCriticalSection(&g_lock);

    // Collapse only argument-less repeats; a call with arguments always prints,
    // because the arguments are the reason it is being logged.
    if (!fmt && g_lastName == name) {
        ++g_repeatCount;
        LeaveCriticalSection(&g_lock);
        return;
    }
    FlushRepeat();
    g_lastName = name;
    g_repeatCount = 1;

    char args[768];
    args[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf(args, sizeof(args), fmt, ap);
        va_end(ap);
        args[sizeof(args) - 1] = 0;
    }

    char buf[1024];
    _snprintf(buf, sizeof(buf) - 2, "[%5u] %s(%s)", ++g_callIndex, name, args);
    buf[sizeof(buf) - 2] = 0;

    size_t len = strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = 0;
    WriteRaw(buf);

    LeaveCriticalSection(&g_lock);
}

void LogHex(const char* label, const void* addr, unsigned int bytes) {
    if (!addr) {
        LogLine("    %s = NULL", label);
        return;
    }
    if (IsBadReadPtr(addr, bytes)) {
        LogLine("    %s = %p (unreadable)", label, addr);
        return;
    }
    const unsigned char* p = (const unsigned char*)addr;
    LogLine("    %s = %p", label, addr);
    for (unsigned int off = 0; off < bytes; off += 16) {
        char hex[64];
        char ascii[24];
        unsigned int n = (bytes - off < 16) ? bytes - off : 16;
        for (unsigned int i = 0; i < n; ++i) {
            _snprintf(hex + i * 3, 4, "%02x ", p[off + i]);
            ascii[i] = (p[off + i] >= 32 && p[off + i] < 127) ? (char)p[off + i] : '.';
        }
        hex[n * 3] = 0;
        ascii[n] = 0;
        LogLine("      +%03x  %-48s |%s|", off, hex, ascii);
    }
}
