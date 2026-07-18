#include "InputHook.hpp"
#include "../core/VRMod.hpp"

namespace bl1gotyvr { namespace input {

InputHook& InputHook::Instance() {
    static InputHook hook;
    return hook;
}

void InputHook::Install() {
    Log("[Input] Install (stub — VR controller injection pending Phase 6)");
}

void InputHook::UpdateState() {
    // TODO: Phase 6 — poll VR controllers via OpenXR action sets
}

}} // namespace bl1gotyvr::input
