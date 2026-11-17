#pragma once

// RemixProxy.log beside the DLL, rewritten every run.
void LogOpen(const char* directory);
void Log(const char* fmt, ...);
