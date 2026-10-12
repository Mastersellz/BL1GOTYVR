#pragma once

#include <Windows.h>
#include <cstdint>

namespace bl1gotyvr::display {

bool Initialize();
void SetGameWindow(HWND window);
void ApplyGameWindowResolution(HWND window);
std::uint32_t TargetResolution();
bool GetPhysicalMonitorRect(HWND window, RECT& rect);

} // namespace bl1gotyvr::display
