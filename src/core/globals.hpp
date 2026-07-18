#pragma once
#include <cstdint>
#include <atomic>

namespace bl1gotyvr {

inline std::atomic<uint64_t> g_frameCount{0};
inline std::atomic<int>      g_currentEye{-1};  // -1=mono, 0=left, 1=right
inline bool                  g_stereoEnabled = true;
inline bool                  g_desktopTestMode = false;

} // namespace bl1gotyvr
