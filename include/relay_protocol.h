#pragma once

#include <stdint.h>

constexpr uint8_t InitializeCommand = 0x02;
constexpr uint8_t ResetCommand = 0x03;
constexpr uint8_t StatusLedOffCommand = 0x04;
constexpr uint8_t ReadCommand = 0x05;
constexpr uint8_t NoRecipientAddress = 0xfe;
constexpr uint8_t BroadcastAddress = 0xff;
constexpr uint8_t NoExecution = 0xff;
constexpr uint8_t MaxStatusCount = 0xfe;

class RelayControl {
public:
    bool command(uint8_t value, uint32_t parameter, uint32_t now)
    {
        switch (value) {
        case 0x00:
            enabled = false;
            timed = false;
            return true;
        case 0xff:
            enabled = true;
            timed = false;
            return true;
        case 0x01:
            enabled = parameter != 0;
            timed = enabled;
            lastUpdate = now;
            remaining = parameter;
            return true;
        default:
            return false;
        }
    }

    void update(uint32_t now)
    {
        if (timed) {
            const uint32_t elapsed = now - lastUpdate;
            lastUpdate = now;
            if (elapsed >= remaining) {
                enabled = false;
                timed = false;
            } else {
                remaining -= elapsed;
            }
        }
    }

    bool isEnabled() const { return enabled; }
    uint32_t status() const { return timed ? remaining : (enabled ? UINT32_MAX : 0); }

private:
    bool enabled = false;
    bool timed = false;
    uint32_t lastUpdate = 0;
    uint32_t remaining = 0;
};

struct NodeResult {
    bool forward;
    uint8_t forwardByte;
    bool frameComplete;
    bool error;
    bool reset;
    bool disableStatusLed;
};

class RelayNode {
public:
    NodeResult receive(uint8_t byte, uint32_t now)
    {
        NodeResult result = {};
        result.forward = !discard;
        result.forwardByte = byte;
        if (position == 0) {
            addressed = byte == 0 || byte == BroadcastAddress;
            commandXor = byte;
            adjustment = 0;
            parameter = 0;
            executed = false;
            pendingReset = false;
            if (byte < NoRecipientAddress) {
                result.forwardByte = byte == 0 ? NoRecipientAddress : byte - 1;
                adjustment = byte ^ result.forwardByte;
            }
        } else if (position < 6) {
            commandXor ^= byte;
            if (position == 1) {
                command = byte;
                if (addressed && command == ReadCommand) {
                    relay.update(now);
                    readValue = relay.status();
                }
            } else {
                parameter = (parameter << 8) | byte;
                if (addressed && command == ReadCommand) {
                    result.forwardByte = static_cast<uint8_t>(readValue >> ((5 - position) * 8));
                    adjustment ^= byte ^ result.forwardByte;
                }
            }
        } else if (position == 6) {
            if (commandXor != byte) {
                result.error = true;
                discard = true;
                pendingReset = true;
                result.forward = false;
            } else {
                result.forwardByte = byte ^ adjustment;
                if (addressed) {
                    if (command == InitializeCommand) {
                        executed = true;
                    } else if (command == ResetCommand) {
                        pendingReset = true;
                        executed = true;
                    } else if (command == StatusLedOffCommand) {
                        result.disableStatusLed = true;
                        executed = true;
                    } else if (command == ReadCommand) {
                        executed = true;
                    } else {
                        executed = relay.command(command, parameter, now);
                        result.error = !executed;
                    }
                }
            }
        } else if (position == 7 && !discard) {
            statusXor = byte;
            count = byte;
            if (count > MaxStatusCount || (executed && count == MaxStatusCount)) {
                result.error = true;
                discard = true;
                result.forward = false;
            } else if (executed) {
                result.forwardByte = byte + 1;
            }
            adjustment = byte ^ result.forwardByte;
        } else if (position == 8 && !discard) {
            statusXor ^= byte;
            if ((count == 0) != (byte == NoExecution) ||
                (!executed && byte == MaxStatusCount)) {
                result.error = true;
                discard = true;
                result.forward = false;
            } else if (executed) {
                result.forwardByte = 0;
            } else if (byte != NoExecution) {
                result.forwardByte = byte + 1;
            }
            adjustment ^= byte ^ result.forwardByte;
        } else if (position == 9) {
            result.frameComplete = true;
            if (!discard && statusXor != byte) {
                result.error = true;
                result.forward = false;
            } else if (!discard) {
                result.forwardByte = byte ^ adjustment;
            }
            result.reset = pendingReset;
            resetParser();
            return result;
        }
        ++position;
        return result;
    }

    bool abort()
    {
        const bool reset = pendingReset;
        resetParser();
        return reset;
    }

    bool receiving() const { return position != 0; }
    void update(uint32_t now) { relay.update(now); }
    bool relayEnabled() const { return relay.isEnabled(); }

private:
    void resetParser() { position = 0; discard = false; pendingReset = false; }

    RelayControl relay;
    uint32_t parameter = 0;
    uint32_t readValue = 0;
    uint8_t position = 0;
    uint8_t command = 0;
    uint8_t commandXor = 0;
    uint8_t statusXor = 0;
    uint8_t adjustment = 0;
    uint8_t count = 0;
    bool addressed = false;
    bool executed = false;
    bool pendingReset = false;
    bool discard = false;
};

class FrameTimeout {
public:
    explicit FrameTimeout(uint32_t ticks) : timeoutTicks(ticks) {}
    void received(uint32_t now) { lastByte = now; }
    bool expired(uint32_t now) const { return now - lastByte >= timeoutTicks; }

private:
    uint32_t timeoutTicks;
    uint32_t lastByte = 0;
};

class ResetAfterTransmit {
public:
    void request() { pending = true; }
    bool waiting() const { return pending; }
    bool ready(bool queueEmpty, bool txEnabled, bool transmitComplete) const
    {
        return pending && queueEmpty && (!txEnabled || transmitComplete);
    }

private:
    bool pending = false;
};
