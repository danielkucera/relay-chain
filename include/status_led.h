#pragma once

#include <stdint.h>

class StatusLed {
public:
    bool update(uint32_t now, bool steadyOn, bool frameReceived)
    {
        if (!steadyOn && static_cast<uint32_t>(now - lastBlink) >= 1000) {
            lastBlink = now;
            heartbeatOn = !heartbeatOn;
        }

        const bool normalState = steadyOn || heartbeatOn;
        if (frameReceived) {
            pulseStarted = now;
            pulseOn = !normalState;
            pulseActive = true;
        }
        if (pulseActive && static_cast<uint32_t>(now - pulseStarted) >= 100) {
            pulseActive = false;
        }
        return pulseActive ? pulseOn : normalState;
    }

private:
    uint32_t lastBlink = 0;
    uint32_t pulseStarted = 0;
    bool heartbeatOn = false;
    bool pulseOn = false;
    bool pulseActive = false;
};
