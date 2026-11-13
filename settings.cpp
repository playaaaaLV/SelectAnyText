#include "settings.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {
constexpr auto settingsPath = L"Software\\SelectAnyText";
constexpr auto previousPath = L"Software\\ScreenCopy";
constexpr UINT allModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

struct StoredSettings {
    DWORD version;
    DWORD modifiers;
    DWORD key;
};

std::wstring KeyLabel(UINT key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return std::wstring(1, static_cast<wchar_t>(key));
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        return L"Num " + std::to_wstring(key - VK_NUMPAD0);
    }
    switch (key) {
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_PAUSE: return L"Pause";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_END: return L"End";
    case VK_HOME: return L"Home";
    case VK_LEFT: return L"Left";
    case VK_UP: return L"Up";
    case VK_RIGHT: return L"Right";
    case VK_DOWN: return L"Down";
    case VK_SNAPSHOT: return L"Print Screen";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_APPS: return L"Menu";
    case VK_MULTIPLY: return L"Num *";
    case VK_ADD: return L"Num +";
    case VK_SUBTRACT: return L"Num -";
    case VK_DECIMAL: return L"Num .";
    case VK_DIVIDE: return L"Num /";
    case VK_NUMLOCK: return L"Num Lock";
    case VK_SCROLL: return L"Scroll Lock";
    case VK_OEM_1: return L";";
    case VK_OEM_PLUS: return L"=";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2: return L"/";
    case VK_OEM_3: return L"`";
    case VK_OEM_4: return L"[";
    case VK_OEM_5: return L"\\";
    case VK_OEM_6: return L"]";
    case VK_OEM_7: return L"'";
    case VK_OEM_102: return L"<";
    }
    wchar_t label[16]{};
    swprintf_s(label, L"Key 0x%02X", key);
    return label;
}

bool ReadStored(const wchar_t* path, StoredSettings& result) {
    std::array<BYTE, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (RegGetValueW(HKEY_CURRENT_USER, path, L"Settings", RRF_RT_REG_BINARY,
            nullptr, buffer.data(), &size) != ERROR_SUCCESS || size < sizeof(result)) {
        return false;
    }
    std::memcpy(&result, buffer.data(), sizeof(result));
    return result.version >= 1 && result.version <= 3;
}
}

UINT ShortcutModifier(UINT key) {
    switch (key) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU: return MOD_ALT;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT: return MOD_SHIFT;
    case VK_LWIN:
    case VK_RWIN: return MOD_WIN;
    }
    return 0;
}

bool IsValidShortcut(Settings shortcut) {
    return shortcut.modifiers && !(shortcut.modifiers & ~allModifiers) &&
        shortcut.key >= VK_BACK && shortcut.key < 0xFF &&
        !ShortcutModifier(shortcut.key) && shortcut.key != VK_PACKET && shortcut.key != VK_PROCESSKEY;
}

std::wstring FormatShortcut(Settings shortcut) {
    std::wstring label;
    if (shortcut.modifiers & MOD_WIN) label += L"Win + ";
    if (shortcut.modifiers & MOD_CONTROL) label += L"Ctrl + ";
    if (shortcut.modifiers & MOD_ALT) label += L"Alt + ";
    if (shortcut.modifiers & MOD_SHIFT) label += L"Shift + ";
    return label + KeyLabel(shortcut.key);
}

Settings ReadSettings() {
    StoredSettings stored{};
    if (!ReadStored(settingsPath, stored) && !ReadStored(previousPath, stored)) {
        return {};
    }
    const Settings shortcut{stored.modifiers, stored.key};
    // Earlier versions could silently save their fallback in place of the requested default.
    const bool previousDefault = (stored.version == 1 && shortcut == Settings{MOD_CONTROL | MOD_ALT, 'E'}) ||
        (stored.version < 3 && shortcut == Settings{MOD_CONTROL | MOD_ALT, VK_F8});
    return IsValidShortcut(shortcut) && !previousDefault ? shortcut : Settings{};
}

bool WriteSettings(Settings settings) {
    const StoredSettings stored{3, settings.modifiers, settings.key};
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settingsPath, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto result = RegSetValueExW(key, L"Settings", 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(&stored), sizeof(stored));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
