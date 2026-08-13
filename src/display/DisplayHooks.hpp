#pragma once

#include <Windows.h>
#include <cstdint>

namespace bl1gotyvr::display {

bool Initialize();
void SetGameWindow(HWND window);
void ApplyGameWindowResolution(HWND window);
std::uint32_t TargetResolution();

} // namespace bl1gotyvr::display
