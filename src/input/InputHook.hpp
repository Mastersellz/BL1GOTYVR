#pragma once

namespace bl1gotyvr { namespace input {

class InputHook {
public:
    static InputHook& Instance();

    void Install();
    void UpdateState();

    // Controller state (public for query)
    float thumbX = 0, thumbY = 0;
    bool trigger = false;
    bool grip = false;
    bool menuButton = false;
};

}} // namespace bl1gotyvr::input
