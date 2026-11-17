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
    float fallback;  // the default of the module that reads it
    float minimum;
    float maximum;
    bool dirty;      // changed on the panel, not yet handed to its module
    bool whole;      // written as a whole number: a checkbox's 0 or 1, or a count of units
};

Setting g_turn = {"Input", "PadTurnSensitivity", 0.6f, 0.6f, 0.1f, 1.0f, false, false};
Setting g_look = {"Input", "PadLookSensitivity", 1.0f, 1.0f, 0.3f, 1.0f, false, false};
Setting g_fov = {"Display", "FieldOfView", 1.0f, 1.0f, 0.7f, 1.6f, false, false};
// The renderer reads it as an integer and takes up to 16384.
Setting g_keepAround = {"FullMap", "KeepAroundCamera", 2048.0f, 2048.0f, 0.0f, 16384.0f, false, true};
// The level's own draw distance, in percent. The ini may go to 400; the slider stops at 200.
Setting g_drawPercent = {"FullMap", "DrawDistancePercent", 100.0f, 100.0f, 100.0f, 400.0f, false, true};
const float kDrawPercentSliderMaximum = 200.0f;
Setting g_stars = {"Sky", "StarrySky", 1.0f, 1.0f, 0.0f, 1.0f, false, true};
Setting* const g_settings[] = {&g_turn, &g_look, &g_fov, &g_keepAround, &g_drawPercent, &g_stars};

char g_ini[MAX_PATH];
float g_projection11 = 0.0f;

// SouthPark_SetSetting(section, key, value), exported by the renderer (Video_D3D.dll) and the
// input shim (dinput.dll): each takes its own keys and returns 0 for the rest.
typedef int (__cdecl *SetSettingFn)(const char* section, const char* key, float value);
const char* const kModules[] = {"Video_D3D.dll", "dinput.dll"};

bool Apply(const Setting& s) {
    bool taken = false;
    for (const char* module : kModules) {
        HMODULE handle = GetModuleHandleA(module);
        SetSettingFn set = handle ? (SetSettingFn)GetProcAddress(handle, "SouthPark_SetSetting") : nullptr;
        if (set && set(s.section, s.key, s.value)) {
            taken = true;
        }
    }
    return taken;
}

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

} // namespace

void Settings::Init(const char* iniPath) {
    lstrcpynA(g_ini, iniPath, sizeof(g_ini));
    for (Setting* s : g_settings) {
        Read(*s);
    }
    Log("settings: pad turn %.2f, pad look %.2f, field of view x%.2f, culling radius %.0f, draw distance "
        "%.0f%%, starry sky %.0f",
        g_turn.value, g_look.value, g_fov.value, g_keepAround.value, g_drawPercent.value, g_stars.value);
}

void Settings::OnSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    // The perspective projection, not the 2D layer's orthographic one.
    if (state == D3DTS_PROJECTION && m && m->_34 != 0.0f) {
        g_projection11 = m->_11;
    }
}

void Settings::Update() {
    for (Setting* s : g_settings) {
        if (s->dirty) {
            if (!Apply(*s)) {
                Log("settings: no module took [%s] %s -- it applies after Save and a restart", s->section, s->key);
            }
            s->dirty = false;
        }
    }
}

bool Settings::WriteKey(const char* section, const char* key, const char* text) {
    char current[64];
    GetPrivateProfileStringA(section, key, "\x01", current, sizeof(current), g_ini);
    if (lstrcmpA(current, text) == 0) {
        return false;
    }
    WritePrivateProfileStringA(section, key, text, g_ini);
    return true;
}

int Settings::Save() {
    int written = 0;
    for (const Setting* s : g_settings) {
        char text[32];
        _snprintf(text, sizeof(text), s->whole ? "%.0f" : "%.2f", s->value);
        text[sizeof(text) - 1] = 0;
        written += WriteKey(s->section, s->key, text) ? 1 : 0;
    }
    return written;
}

void Settings::ResetToDefaults() {
    for (Setting* s : g_settings) {
        s->value = s->fallback;
        s->dirty = true;
    }
}

void Settings::DrawPanel() {
    Ui::Heading("Game");
    g_turn.dirty |= Ui::Slider("Pad turn sensitivity", &g_turn.value, g_turn.minimum, g_turn.maximum, "%.2f");
    g_look.dirty |= Ui::Slider("Pad look sensitivity", &g_look.value, g_look.minimum, g_look.maximum, "%.2f");
    g_fov.dirty |= Ui::Slider("Field of view", &g_fov.value, g_fov.minimum, g_fov.maximum, "x%.2f");
    // KeepAroundCamera: the level is drawn within this radius whichever way the camera faces.
    g_keepAround.dirty |= Ui::Slider("Culling radius (drawn behind the camera too)", &g_keepAround.value,
                                     g_keepAround.minimum, g_keepAround.maximum, "%.0f units");
    // DrawDistancePercent: how far ahead the level is drawn, against its own distance. A value
    // above the slider's end, set in the ini, shows as a full bar and stays until dragged.
    if (Ui::Slider("Draw distance (of the level's own)", &g_drawPercent.value, g_drawPercent.minimum,
                   kDrawPercentSliderMaximum, "%.0f%%")) {
        g_drawPercent.value = floorf(g_drawPercent.value + 0.5f);
        g_drawPercent.dirty = true;
    }
    bool starry = g_stars.value != 0.0f;
    if (Ui::Checkbox("Starry sky: the intro's and menus' star cards become the sky", &starry)) {
        g_stars.value = starry ? 1.0f : 0.0f;
        g_stars.dirty = true;
    }
    if (g_projection11 != 0.0f) {
        const float degrees = 2.0f * atanf(1.0f / fabsf(g_projection11)) * 57.29578f;
        Ui::LabelDim("Field of view now: %.0f degrees across.", degrees);
    }
}
