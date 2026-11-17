#include "settings.h"
#include "log.h"
#include "overlay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

struct Setting {
    const char* section;
    const char* key;
    float value;
    float minimum;
    float maximum;
    bool dirty;      // changed on the panel, not yet written
    bool onOff;      // a checkbox, written as 0 or 1
};

// Defaults match the modules that read them.
Setting g_turn = {"Input", "PadTurnSensitivity", 0.6f, 0.1f, 1.0f, false, false};
Setting g_look = {"Input", "PadLookSensitivity", 1.0f, 0.3f, 1.0f, false, false};
Setting g_fov = {"Display", "FieldOfView", 1.0f, 0.7f, 1.6f, false, false};
Setting g_stars = {"Sky", "StarrySky", 1.0f, 0.0f, 1.0f, false, true};
Setting* const g_settings[] = {&g_turn, &g_look, &g_fov, &g_stars};

char g_ini[MAX_PATH];
DWORD g_nextWrite = 0;
// A drag changes the value every frame; the file is written at most this often.
const DWORD kWriteIntervalMs = 250;

float g_projection11 = 0.0f;

void Read(Setting& s) {
    char text[32];
    GetPrivateProfileStringA(s.section, s.key, "", text, sizeof(text), g_ini);
    if (text[0]) {
        const float value = (float)atof(text);
        if (value >= s.minimum && value <= s.maximum) {
            s.value = value;
        }
    }
}

void Write(const Setting& s) {
    char text[32];
    _snprintf(text, sizeof(text), s.onOff ? "%.0f" : "%.2f", s.value);
    text[sizeof(text) - 1] = 0;
    WritePrivateProfileStringA(s.section, s.key, text, g_ini);
}

} // namespace

void Settings::Init(const char* iniPath) {
    lstrcpynA(g_ini, iniPath, sizeof(g_ini));
    for (Setting* s : g_settings) {
        Read(*s);
    }
    Log("settings: pad turn %.2f, pad look %.2f, field of view x%.2f, starry sky %.0f", g_turn.value,
        g_look.value, g_fov.value, g_stars.value);
}

void Settings::OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    // The perspective projection, not the 2D layer's orthographic one.
    if (state == D3DTS_PROJECTION && m && m->_34 != 0.0f) {
        g_projection11 = m->_11;
    }
}

// Only what the panel changed is written: the others may have been edited in the file since.
void Settings::Update() {
    if ((int)(GetTickCount() - g_nextWrite) < 0) {
        return;
    }
    for (Setting* s : g_settings) {
        if (s->dirty) {
            Write(*s);
            s->dirty = false;
            g_nextWrite = GetTickCount() + kWriteIntervalMs;
        }
    }
}

void Settings::DrawPanel() {
    Ui::Heading("Game");
    g_turn.dirty |= Ui::Slider("Pad turn sensitivity", &g_turn.value, g_turn.minimum, g_turn.maximum, "%.2f");
    g_look.dirty |= Ui::Slider("Pad look sensitivity", &g_look.value, g_look.minimum, g_look.maximum, "%.2f");
    g_fov.dirty |= Ui::Slider("Field of view", &g_fov.value, g_fov.minimum, g_fov.maximum, "x%.2f");
    bool starry = g_stars.value != 0.0f;
    if (Ui::Checkbox("Starry sky: the intro's and menus' star cards become the sky", &starry)) {
        g_stars.value = starry ? 1.0f : 0.0f;
        g_stars.dirty = true;
    }
    if (g_projection11 != 0.0f) {
        const float degrees = 2.0f * atanf(1.0f / fabsf(g_projection11)) * 57.29578f;
        Ui::LabelDim("Field of view now: %.0f degrees across. Saved to Video_D3D9.ini as you go.", degrees);
    } else {
        Ui::LabelDim("Saved to Video_D3D9.ini as you go.");
    }
}
