#pragma once

#include <windows.h>

#include <string>

struct Settings {
    UINT modifiers = MOD_WIN | MOD_SHIFT;
    UINT key = 'D';

    bool operator==(const Settings&) const = default;
};

UINT ShortcutModifier(UINT key);
bool IsValidShortcut(Settings shortcut);
std::wstring FormatShortcut(Settings shortcut);
Settings ReadSettings();
bool WriteSettings(Settings settings);
