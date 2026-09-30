#pragma once

constexpr int kTouchControlsMaxKeepOuts = 4;

struct TouchControlsKeepOut {
    float left;
    float top;
    float right;
    float bottom;
};

struct TouchControlsSafeArea {
    float left;
    float top;
    float right;
    float bottom;
    int keepOutCount;
    TouchControlsKeepOut keepOuts[kTouchControlsMaxKeepOuts];
    int divisionCount;
    TouchControlsKeepOut divisions[kTouchControlsMaxKeepOuts];
};

TouchControlsSafeArea TouchControls_IosSafeArea();
