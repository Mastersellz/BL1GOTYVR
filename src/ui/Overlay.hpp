#pragma once

struct IDXGISwapChain;

namespace bl1gotyvr { namespace ui {

void OnPresent(IDXGISwapChain* swapChain);
bool IsVisible();

}} // namespace bl1gotyvr::ui
