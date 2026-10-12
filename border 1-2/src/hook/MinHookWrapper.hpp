#pragma once
#include <MinHook.h>

// MinHook auto-init via static object
namespace bl1gotyvr { namespace hook {

struct MinHookInit {
    MinHookInit() { MH_Initialize(); }
    ~MinHookInit() { MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); }
};

// Will be instantiated in VRMod.cpp before hooks are installed
inline MinHookInit s_minhookInit;

}} // namespace bl1gotyvr::hook
