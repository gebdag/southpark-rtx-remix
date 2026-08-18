#pragma once

// The renderer's log. Every line is flushed as it is written, so a crash keeps its tail; with
// [Log] LogEveryCall, every export call is logged too.

void LogOpen(const char* path);
void LogClose();
void LogLine(const char* fmt, ...);

// Records an export entry with a monotonic call index. Repeated calls to the same
// per-frame export collapse into a count so the log stays readable across a session.
//
// Off unless the ini turns it on: the 2D layer alone makes about 1.6 million calls per run,
// which costs gigabytes of log and a good deal of frame rate.
void LogCall(const char* name, const char* fmt = 0, ...);
void LogSetCallLogging(bool enabled);

// Hex + ASCII dump of a caller-supplied buffer, guarded against unreadable pointers.
void LogHex(const char* label, const void* addr, unsigned int bytes);
