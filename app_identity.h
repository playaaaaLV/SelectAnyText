#pragma once

namespace identity {
inline constexpr auto appName = L"SelectAnyText";
inline constexpr auto trayClass = L"SelectAnyText.Tray";
inline constexpr auto overlayClass = L"SelectAnyText.Selection";
// Keep the original identity so upgrades cannot run two main instances.
inline constexpr auto mutexName = L"Local\\ScreenCopy.Instance";
}
