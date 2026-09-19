#pragma once

#include <cstdint>

namespace bl1gotyvr::reshade {

// Shared memory block published by the ReShade add-on (see tools/reshade_capture)
// and consumed by the mod's stereo capture path. The add-on copies the final
// post-effect back buffer into a shared texture and publishes its NT handle, so
// the mod opens its own reference with the game's D3D11 device.
constexpr wchar_t kSharedFrameName[] = L"Local\\BL1GOTYVR_ReShadeFrame";
constexpr uint32_t kSharedFrameMagic = 0x424C3152; // 'BL1R'

#pragma pack(push, 8)
struct SharedFrame {
    volatile int32_t sequence; // seqlock: odd while being written
    int32_t magic;
    uint64_t frameSerial;
    uint64_t sharedHandle; // exported NT handle of the copy texture
    uint32_t width;
    uint32_t height;
    uint32_t format; // native DXGI_FORMAT, informational
    uint32_t reserved;
};
#pragma pack(pop)

} // namespace bl1gotyvr::reshade
