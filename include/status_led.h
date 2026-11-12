#pragma once

#include <stdint.h>

class StatusLed {
public:
    void disable() { disabled = true; }

    bool update(uint32_t now, bool steadyOn, bool frameReceived, bool initialized)
    {
        if (disabled) {
            return false;
        }
        if (initialized && !wasInitialized) {
            wasInitialized = true;
            lastBlink = now;
            heartbeatStarted = false;
        }
        const uint32_t interval = initialized ? 2000 : 1000;
        if (!steadyOn && static_cast<uint32_t>(now - lastBlink) >= interval) {
            lastBlink = now;
            heartbeatStarted = initialized || !heartbeatStarted;
        }

        const bool normalState = steadyOn ||
            (heartbeatStarted &&
             (!initialized || static_cast<uint32_t>(now - lastBlink) < 100));
        if (frameReceived) {
            pulseStarted = now;
            pulseOn = !normalState;
            pulseActive = true;
        }
        if (pulseActive && static_cast<uint32_t>(now - pulseStarted) >= 40) {
            pulseActive = false;
        }
        return pulseActive ? pulseOn : normalState;
    }

private:
    uint32_t lastBlink = 0;
    uint32_t pulseStarted = 0;
    bool heartbeatStarted = false;
    bool wasInitialized = false;
    bool pulseOn = false;
    bool pulseActive = false;
    bool disabled = false;
};
