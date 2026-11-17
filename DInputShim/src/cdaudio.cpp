#include "cdaudio.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

// The game opens MCI's cdaudio device by type (FUN_00470900: MCI_OPEN with MCI_OPEN_TYPE_ID and
// MCI_DEVTYPE_CD_AUDIO), sets the TMSF time format, asks for the number of tracks and their
// lengths (its own disc check, FUN_004704d0, wants 14 tracks and a 5:24-5:27 track 2), and plays
// one track at a time from its start to its length with MCI_NOTIFY. Some of those calls go
// through the copy protection's import dispatcher, so the executable's import table does not see
// them all; winmm's own mciSendCommandA does. It is hot-patchable (`mov edi, edi` with five bytes
// of padding in front), so a jump goes in front of it and the original runs from two bytes in.
//
// Every command for another device, and every open that is not cdaudio, goes to the original.

namespace {

typedef MCIERROR (WINAPI* MciSendCommandFn)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);

CdAudio::LogFn g_log = nullptr;
MciSendCommandFn g_realSend = nullptr;

const MCIDEVICEID kDevice = 0x5350;    // ours; MCI numbers its own devices from 1
const int kMaxTracks = 99;
const DWORD kCdFrames = 75;            // CD frames a second, the unit of MSF and TMSF
const int kBuffers = 4;
const DWORD kBufferMs = 250;

struct Track {
    bool present = false;
    char path[MAX_PATH] = {};
    WAVEFORMATEX format = {};
    DWORD dataOffset = 0;
    DWORD dataBytes = 0;
};

Track g_tracks[kMaxTracks + 1];
int g_lastTrack = 0;

// The device, as the game sees it. Written on the game's thread, read by the player.
enum Mode { kStopped, kPlaying, kPaused };
CRITICAL_SECTION g_lock;
bool g_open = false;
DWORD g_timeFormat = MCI_FORMAT_MSF;
Mode g_mode = kStopped;
int g_track = 1;               // where playback is or will start: track ...
DWORD g_trackSample = 0;       // ... and sample frame within it
int g_endTrack = 0;            // where it stops: track and sample frame (0 = its end)
DWORD g_endSample = 0;
HWND g_notify = nullptr;       // MCI_NOTIFY target of the running play
unsigned int g_order = 0;      // bumped by every play, stop and seek
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
HWAVEOUT g_out = nullptr;      // the player's output while a track streams ...
DWORD g_streamStart = 0;       // ... which began at this sample frame of g_track

// ------------------------------------------------------------------------------ tracks

bool ReadWav(const char* path, Track& t) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    DWORD got = 0;
    char riff[12];
    if (ReadFile(f, riff, 12, &got, nullptr) && got == 12 && !memcmp(riff, "RIFF", 4) &&
        !memcmp(riff + 8, "WAVE", 4)) {
        bool haveFormat = false;
        for (;;) {
            char id[4];
            DWORD size = 0;
            if (!ReadFile(f, id, 4, &got, nullptr) || got != 4 || !ReadFile(f, &size, 4, &got, nullptr) || got != 4) {
                break;
            }
            const DWORD at = SetFilePointer(f, 0, nullptr, FILE_CURRENT);
            if (!memcmp(id, "fmt ", 4)) {
                PCMWAVEFORMAT pcm = {};
                ReadFile(f, &pcm, size < sizeof(pcm) ? size : sizeof(pcm), &got, nullptr);
                t.format.wFormatTag = pcm.wf.wFormatTag;
                t.format.nChannels = pcm.wf.nChannels;
                t.format.nSamplesPerSec = pcm.wf.nSamplesPerSec;
                t.format.nAvgBytesPerSec = pcm.wf.nAvgBytesPerSec;
                t.format.nBlockAlign = pcm.wf.nBlockAlign;
                t.format.wBitsPerSample = pcm.wBitsPerSample;
                t.format.cbSize = 0;
                haveFormat = true;
            } else if (!memcmp(id, "data", 4)) {
                const DWORD fileSize = GetFileSize(f, nullptr);
                t.dataOffset = at;
                t.dataBytes = size < fileSize - at ? size : fileSize - at;
                ok = haveFormat && t.format.wFormatTag == WAVE_FORMAT_PCM && t.format.nBlockAlign &&
                     t.format.nSamplesPerSec;
                break;
            }
            SetFilePointer(f, at + size + (size & 1), nullptr, FILE_BEGIN);
        }
    }
    CloseHandle(f);
    if (ok) {
        lstrcpynA(t.path, path, MAX_PATH);
        t.present = true;
    }
    return ok;
}

DWORD Samples(int track) {
    const Track& t = g_tracks[track];
    return t.present ? t.dataBytes / t.format.nBlockAlign : 0;
}

DWORD Rate(int track) {
    return g_tracks[track].present ? g_tracks[track].format.nSamplesPerSec : 44100;
}

DWORD FramesFromSamples(int track, DWORD samples) {
    return (DWORD)((unsigned long long)samples * kCdFrames / Rate(track));
}

DWORD SamplesFromFrames(int track, DWORD frames) {
    return (DWORD)((unsigned long long)frames * Rate(track) / kCdFrames);
}

DWORD Msf(DWORD frames) {
    return MCI_MAKE_MSF(frames / (60 * kCdFrames), (frames / kCdFrames) % 60, frames % kCdFrames);
}

DWORD FramesFromMsf(DWORD msf) {
    return (MCI_MSF_MINUTE(msf) * 60 + MCI_MSF_SECOND(msf)) * kCdFrames + MCI_MSF_FRAME(msf);
}

// ------------------------------------------------------------------------------ player

struct Stream {
    HWAVEOUT out = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD left = 0;            // bytes still to queue
    DWORD bufferBytes = 0;
    WAVEHDR headers[kBuffers] = {};
    char* memory[kBuffers] = {};
};

void CloseStream(Stream& s) {
    EnterCriticalSection(&g_lock);
    g_out = nullptr;
    LeaveCriticalSection(&g_lock);
    if (s.out) {
        waveOutReset(s.out);
        for (WAVEHDR& h : s.headers) {
            if (h.dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(s.out, &h, sizeof(h));
            }
            h = WAVEHDR();
        }
        waveOutClose(s.out);
        s.out = nullptr;
    }
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    for (char*& m : s.memory) {
        delete[] m;
        m = nullptr;
    }
}

// Opens `track` from sample frame `sample` up to `endSample` (0: its end).
bool OpenStream(Stream& s, int track, DWORD sample, DWORD endSample, HANDLE done) {
    const Track& t = g_tracks[track];
    s.file = CreateFileA(t.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD align = t.format.nBlockAlign;
    const DWORD total = Samples(track);
    const DWORD from = sample < total ? sample : total;
    const DWORD to = (endSample && endSample < total) ? endSample : total;
    SetFilePointer(s.file, t.dataOffset + from * align, nullptr, FILE_BEGIN);
    s.left = to > from ? (to - from) * align : 0;
    WAVEFORMATEX format = t.format;
    const MMRESULT opened = waveOutOpen(&s.out, WAVE_MAPPER, &format, (DWORD_PTR)done, 0, CALLBACK_EVENT);
    if (opened != MMSYSERR_NOERROR) {
        g_log("cdaudio: track %d: waveOutOpen failed (%u)", track, opened);
        s.out = nullptr;
        return false;
    }
    g_log("cdaudio: track %d playing from %u:%02u", track, from / t.format.nSamplesPerSec / 60,
          from / t.format.nSamplesPerSec % 60);
    s.bufferBytes = t.format.nAvgBytesPerSec * kBufferMs / 1000 / align * align;
    for (char*& m : s.memory) {
        m = new char[s.bufferBytes];
    }
    EnterCriticalSection(&g_lock);
    g_out = s.out;
    g_streamStart = from;
    LeaveCriticalSection(&g_lock);
    return true;
}

// Queues what has been played out again; false once everything queued has played.
bool Feed(Stream& s) {
    bool busy = false;
    for (int i = 0; i < kBuffers; ++i) {
        WAVEHDR& h = s.headers[i];
        if (h.dwFlags & WHDR_PREPARED) {
            if (!(h.dwFlags & WHDR_DONE)) {
                busy = true;
                continue;
            }
            waveOutUnprepareHeader(s.out, &h, sizeof(h));
        }
        if (!s.left) {
            continue;
        }
        DWORD got = 0;
        ReadFile(s.file, s.memory[i], s.bufferBytes < s.left ? s.bufferBytes : s.left, &got, nullptr);
        if (!got) {
            s.left = 0;
            continue;
        }
        s.left -= got;
        h = WAVEHDR();
        h.lpData = s.memory[i];
        h.dwBufferLength = got;
        waveOutPrepareHeader(s.out, &h, sizeof(h));
        waveOutWrite(s.out, &h, sizeof(h));
        busy = true;
    }
    return busy;
}

int NextTrack(int track, int last) {
    for (int t = track + 1; t <= last; ++t) {
        if (g_tracks[t].present) {
            return t;
        }
    }
    return 0;
}

DWORD WINAPI Player(void*) {
    HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    Stream s;
    unsigned int seen = 0;
    Mode heard = kStopped;
    for (;;) {
        EnterCriticalSection(&g_lock);
        const unsigned int order = g_order;
        const Mode mode = g_mode;
        int track = g_track;
        const DWORD sample = g_trackSample;
        const int endTrack = g_endTrack;
        const DWORD endSample = g_endSample;
        LeaveCriticalSection(&g_lock);

        if (order != seen) {
            seen = order;
            CloseStream(s);
            heard = kPlaying;
            if (mode != kStopped) {
                if (!g_tracks[track].present) {
                    track = NextTrack(track, endTrack);
                }
                if (!track || !OpenStream(s, track, sample, track == endTrack ? endSample : 0, done)) {
                    CloseStream(s);
                }
            }
        }
        if (s.out && mode != heard) {
            mode == kPaused ? waveOutPause(s.out) : waveOutRestart(s.out);
            heard = mode;
        }
        if (mode == kPlaying && s.out && Feed(s)) {
            HANDLE waits[2] = {done, g_wake};
            WaitForMultipleObjects(2, waits, FALSE, 100);
            continue;
        }
        if (mode == kPlaying && order == seen) {
            // The stream ran out: the next track of the range, or the end of it.
            CloseStream(s);
            const int next = track < endTrack ? NextTrack(track, endTrack) : 0;
            HWND notify = nullptr;
            EnterCriticalSection(&g_lock);
            if (g_order == seen) {
                if (next) {
                    g_track = next;
                    g_trackSample = 0;
                } else {
                    g_mode = kStopped;
                    g_track = track;
                    g_trackSample = Samples(track);
                    notify = g_notify;
                    g_notify = nullptr;
                }
            }
            LeaveCriticalSection(&g_lock);
            if (next && OpenStream(s, next, 0, next == endTrack ? endSample : 0, done)) {
                continue;
            }
            CloseStream(s);
            g_log("cdaudio: track %d finished%s", track, notify ? ", game notified" : "");
            if (notify) {
                PostMessageA(notify, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, kDevice);
            }
            continue;
        }
        WaitForSingleObject(g_wake, INFINITE);
    }
}

// ------------------------------------------------------------------------------ the device

// Called with g_lock held. A play that is replaced or stopped tells its window so.
void AbandonNotify(WPARAM why) {
    if (g_notify) {
        PostMessageA(g_notify, MM_MCINOTIFY, why, kDevice);
        g_notify = nullptr;
    }
}

void Order() {
    ++g_order;
    SetEvent(g_wake);
}

// The current position as the game's time format asks for it.
DWORD Position() {
    int track = g_track;
    DWORD sample = g_trackSample;
    if (g_out && g_mode != kStopped) {
        MMTIME time = {};
        time.wType = TIME_SAMPLES;
        waveOutGetPosition(g_out, &time, sizeof(time));
        sample = g_streamStart + (time.wType == TIME_SAMPLES ? time.u.sample : 0);
    }
    const DWORD frames = FramesFromSamples(track, sample);
    if (g_timeFormat == MCI_FORMAT_TMSF) {
        return MCI_MAKE_TMSF(track, frames / (60 * kCdFrames), (frames / kCdFrames) % 60, frames % kCdFrames);
    }
    DWORD before = 0;
    for (int t = 1; t < track; ++t) {
        before += FramesFromSamples(t, Samples(t));
    }
    return g_timeFormat == MCI_FORMAT_MILLISECONDS ? (before + frames) * 1000 / kCdFrames : Msf(before + frames);
}

// A position the game gives, in its time format, as a track and a sample frame within it.
void Locate(DWORD value, int* track, DWORD* sample) {
    if (g_timeFormat == MCI_FORMAT_TMSF) {
        *track = MCI_TMSF_TRACK(value);
        const DWORD frames = (MCI_TMSF_MINUTE(value) * 60 + MCI_TMSF_SECOND(value)) * kCdFrames + MCI_TMSF_FRAME(value);
        *sample = SamplesFromFrames(*track, frames);
        return;
    }
    DWORD frames = g_timeFormat == MCI_FORMAT_MILLISECONDS ? value * kCdFrames / 1000 : FramesFromMsf(value);
    for (int t = 1; t <= g_lastTrack; ++t) {
        const DWORD length = FramesFromSamples(t, Samples(t));
        if (frames < length || t == g_lastTrack) {
            *track = t;
            *sample = SamplesFromFrames(t, frames);
            return;
        }
        frames -= length;
    }
}

MCIERROR Status(DWORD_PTR flags, MCI_STATUS_PARMS* p) {
    if (!(flags & MCI_STATUS_ITEM)) {
        return MCIERR_MISSING_PARAMETER;
    }
    const int track = (flags & MCI_TRACK) ? (int)p->dwTrack : 0;
    if ((flags & MCI_TRACK) && (track < 1 || track > g_lastTrack)) {
        return MCIERR_OUTOFRANGE;
    }
    switch (p->dwItem) {
    case MCI_STATUS_NUMBER_OF_TRACKS: p->dwReturn = g_lastTrack; break;
    case MCI_STATUS_LENGTH: {
        DWORD frames = 0;
        for (int t = track ? track : 1; t <= (track ? track : g_lastTrack); ++t) {
            frames += FramesFromSamples(t, Samples(t));
        }
        p->dwReturn = g_timeFormat == MCI_FORMAT_MILLISECONDS ? frames * 1000 / kCdFrames : Msf(frames);
        break;
    }
    case MCI_STATUS_POSITION:
        p->dwReturn = track ? (g_timeFormat == MCI_FORMAT_TMSF ? MCI_MAKE_TMSF(track, 0, 0, 0) : 0) : Position();
        break;
    case MCI_STATUS_MODE:
        p->dwReturn = g_mode == kPlaying ? MCI_MODE_PLAY : g_mode == kPaused ? MCI_MODE_PAUSE : MCI_MODE_STOP;
        break;
    case MCI_STATUS_MEDIA_PRESENT:
    case MCI_STATUS_READY: p->dwReturn = TRUE; break;
    case MCI_STATUS_TIME_FORMAT: p->dwReturn = g_timeFormat; break;
    case MCI_STATUS_CURRENT_TRACK: p->dwReturn = g_track; break;
    case MCI_CDA_STATUS_TYPE_TRACK:
        p->dwReturn = g_tracks[track ? track : g_track].present ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER;
        break;
    default: return MCIERR_UNSUPPORTED_FUNCTION;
    }
    return 0;
}

MCIERROR Play(DWORD_PTR flags, MCI_PLAY_PARMS* p) {
    if (g_mode == kPaused && !(flags & (MCI_FROM | MCI_TO))) {
        g_mode = kPlaying;
        SetEvent(g_wake);
        return 0;
    }
    int fromTrack = g_track, toTrack = g_lastTrack;
    DWORD fromSample = g_trackSample, toSample = 0;
    if (flags & MCI_FROM) {
        Locate(p->dwFrom, &fromTrack, &fromSample);
    }
    if (flags & MCI_TO) {
        Locate(p->dwTo, &toTrack, &toSample);
    }
    if (fromTrack < 1 || fromTrack > g_lastTrack || toTrack < fromTrack) {
        return MCIERR_OUTOFRANGE;
    }
    AbandonNotify(MCI_NOTIFY_SUPERSEDED);
    g_track = fromTrack;
    g_trackSample = fromSample;
    g_endTrack = toTrack;
    g_endSample = toSample;
    g_notify = (flags & MCI_NOTIFY) ? (HWND)p->dwCallback : nullptr;
    g_mode = kPlaying;
    if (!g_thread) {
        g_thread = CreateThread(nullptr, 0, &Player, nullptr, 0, nullptr);
    }
    Order();
    return 0;
}

// Stops where playback is, so a later play without a start resumes from there.
void Stop() {
    AbandonNotify(MCI_NOTIFY_ABORTED);
    if (g_mode != kStopped && g_out) {
        MMTIME time = {};
        time.wType = TIME_SAMPLES;
        waveOutGetPosition(g_out, &time, sizeof(time));
        g_trackSample = g_streamStart + (time.wType == TIME_SAMPLES ? time.u.sample : 0);
    }
    g_mode = kStopped;
    Order();
}

MCIERROR Command(UINT message, DWORD_PTR flags, DWORD_PTR params) {
    MCIERROR result = 0;
    EnterCriticalSection(&g_lock);
    switch (message) {
    case MCI_CLOSE:
        Stop();
        g_open = false;
        break;
    case MCI_SET:
        if (flags & MCI_SET_TIME_FORMAT) {
            g_timeFormat = ((MCI_SET_PARMS*)params)->dwTimeFormat;
        }
        break;
    case MCI_STATUS: result = Status(flags, (MCI_STATUS_PARMS*)params); break;
    case MCI_PLAY: result = Play(flags, (MCI_PLAY_PARMS*)params); break;
    case MCI_STOP: Stop(); break;
    case MCI_PAUSE:
        if (g_mode == kPlaying) {
            g_mode = kPaused;
            SetEvent(g_wake);
        }
        break;
    case MCI_RESUME:
        if (g_mode == kPaused) {
            g_mode = kPlaying;
            SetEvent(g_wake);
        }
        break;
    case MCI_SEEK: {
        Stop();
        MCI_SEEK_PARMS* p = (MCI_SEEK_PARMS*)params;
        if (flags & MCI_SEEK_TO_END) {
            g_track = g_lastTrack;
            g_trackSample = Samples(g_lastTrack);
        } else if (flags & MCI_TO) {
            Locate(p->dwTo, &g_track, &g_trackSample);
        } else {
            g_track = 1;
            g_trackSample = 0;
        }
        break;
    }
    case MCI_GETDEVCAPS: {
        MCI_GETDEVCAPS_PARMS* p = (MCI_GETDEVCAPS_PARMS*)params;
        p->dwReturn = p->dwItem == MCI_GETDEVCAPS_DEVICE_TYPE ? MCI_DEVTYPE_CD_AUDIO
                      : p->dwItem == MCI_GETDEVCAPS_CAN_RECORD || p->dwItem == MCI_GETDEVCAPS_CAN_SAVE ||
                              p->dwItem == MCI_GETDEVCAPS_USES_FILES || p->dwItem == MCI_GETDEVCAPS_COMPOUND_DEVICE
                          ? FALSE
                          : TRUE;
        break;
    }
    case MCI_INFO: {
        MCI_INFO_PARMSA* p = (MCI_INFO_PARMSA*)params;
        if (p->lpstrReturn && p->dwRetSize) {
            lstrcpynA(p->lpstrReturn, "South Park music folder", p->dwRetSize);
        }
        break;
    }
    default: result = MCIERR_UNSUPPORTED_FUNCTION; break;
    }
    LeaveCriticalSection(&g_lock);
    if (!result && message != MCI_PLAY && (flags & MCI_NOTIFY)) {
        PostMessageA((HWND)((MCI_GENERIC_PARMS*)params)->dwCallback, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, kDevice);
    }
    return result;
}

bool OpensCdAudio(DWORD_PTR flags, DWORD_PTR params) {
    const MCI_OPEN_PARMSA* p = (const MCI_OPEN_PARMSA*)params;
    if (!p || !(flags & MCI_OPEN_TYPE)) {
        return false;
    }
    if (flags & MCI_OPEN_TYPE_ID) {
        return LOWORD((DWORD_PTR)p->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO;
    }
    return p->lpstrDeviceType && lstrcmpiA(p->lpstrDeviceType, "cdaudio") == 0;
}

MCIERROR WINAPI SendCommand(MCIDEVICEID device, UINT message, DWORD_PTR flags, DWORD_PTR params) {
    if (message == MCI_OPEN && OpensCdAudio(flags, params)) {
        EnterCriticalSection(&g_lock);
        g_open = true;
        g_timeFormat = MCI_FORMAT_MSF;
        LeaveCriticalSection(&g_lock);
        ((MCI_OPEN_PARMSA*)params)->wDeviceID = kDevice;
        if (flags & MCI_NOTIFY) {
            PostMessageA((HWND)((MCI_OPEN_PARMSA*)params)->dwCallback, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, kDevice);
        }
        return 0;
    }
    if (device == kDevice) {
        return Command(message, flags, params);
    }
    if (message == MCI_CLOSE && device == MCI_ALL_DEVICE_ID) {
        Command(MCI_CLOSE, 0, 0);
    }
    return g_realSend(device, message, flags, params);
}

// jmp SendCommand in the five bytes in front, then a short jump back to it over `mov edi, edi`.
bool HotPatch(unsigned char* function, void* replacement) {
    static const unsigned char kMovEdiEdi[2] = {0x8B, 0xFF};
    if (IsBadReadPtr(function - 5, 7) || memcmp(function, kMovEdiEdi, 2) != 0) {
        return false;
    }
    for (int i = 1; i <= 5; ++i) {
        if (function[-i] != 0xCC && function[-i] != 0x90) {
            return false;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(function - 5, 7, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    const int rel = (int)((unsigned char*)replacement - function);
    function[-5] = 0xE9;
    memcpy(function - 4, &rel, 4);
    *(volatile unsigned short*)function = 0xF9EB;     // jmp short -7
    VirtualProtect(function - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), function - 5, 7);
    return true;
}

} // namespace

void CdAudio::Install(const char* gameFolder, LogFn log) {
    g_log = log;
    for (int track = 1; track <= kMaxTracks; ++track) {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%smusic\\track%02d.wav", gameFolder, track);
        path[sizeof(path) - 1] = 0;
        if (ReadWav(path, g_tracks[track])) {
            g_lastTrack = track;
        }
    }
    if (!g_lastTrack) {
        log("cdaudio: no music\\trackNN.wav -- the game's CD audio goes to the drive");
        return;
    }
    int present = 0;
    for (int track = 1; track <= g_lastTrack; ++track) {
        present += g_tracks[track].present ? 1 : 0;
    }
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    unsigned char* send = winmm ? (unsigned char*)GetProcAddress(winmm, "mciSendCommandA") : nullptr;
    if (!send) {
        log("cdaudio: winmm is not resident -- the game's CD audio goes to the drive");
        return;
    }
    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_realSend = (MciSendCommandFn)(send + 2);
    if (!HotPatch(send, (void*)&SendCommand)) {
        log("cdaudio: winmm's mciSendCommandA is not the expected hot-patchable entry -- the game's "
            "CD audio goes to the drive");
        return;
    }
    log("cdaudio: %d tracks from the music folder (up to track %d) stand in for the CD", present, g_lastTrack);
}
