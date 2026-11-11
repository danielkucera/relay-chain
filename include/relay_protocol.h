#pragma once

#include <stdint.h>

constexpr uint8_t InitializeCommand = 0x02;
constexpr uint8_t BroadcastAddress = 0xff;

struct ProtocolResult {
    bool forward;
    uint8_t forwardByte;
    bool frameComplete;
    bool localComplete;
    bool checksumValid;
    uint8_t command;
    uint32_t parameter;
};

class RelayProtocol {
public:
    ProtocolResult receive(uint8_t byte)
    {
        ProtocolResult result = {};
        if (position == 0) {
            forwardFrame = byte != 0;
            broadcastFrame = byte == BroadcastAddress;
            if (forwardFrame && !broadcastFrame) {
                const uint8_t nextAddress = byte - 1;
                checksumAdjustment = byte ^ nextAddress;
            }
            checksum = 0;
            command = 0;
            parameter = 0;
        }

        result.forward = forwardFrame;
        result.frameComplete = position == 6;
        if (forwardFrame) {
            if (broadcastFrame) {
                result.forwardByte = byte;
            } else if (position == 0) {
                result.forwardByte = byte - 1;
            } else if (position == 6) {
                result.forwardByte = byte ^ checksumAdjustment;
            } else {
                result.forwardByte = byte;
            }
        }
        if (position < 6) {
            checksum ^= byte;
            if (position == 1) {
                command = byte;
            } else if (position >= 2) {
                parameter = (parameter << 8) | byte;
            }
        } else if (!forwardFrame || broadcastFrame) {
            result.localComplete = true;
            result.checksumValid = checksum == byte;
            result.command = command;
            result.parameter = parameter;
        }

        position = (position + 1) % 7;
        return result;
    }

    void reset() { position = 0; }

private:
    uint8_t position = 0;
    uint8_t checksum = 0;
    uint8_t command = 0;
    uint8_t checksumAdjustment = 0;
    uint32_t parameter = 0;
    bool forwardFrame = false;
    bool broadcastFrame = false;
};

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
};

class RelayNode {
public:
    NodeResult receive(uint8_t byte, uint32_t now)
    {
        const ProtocolResult received = protocol.receive(byte);
        NodeResult result = {};
        result.forward = received.forward;
        result.forwardByte = received.forwardByte;
        result.frameComplete = received.frameComplete;
        if (received.localComplete) {
            if (!received.checksumValid ||
                (received.command != InitializeCommand &&
                 !relay.command(received.command, received.parameter, now))) {
                result.error = true;
            } else if (received.command == InitializeCommand) {
                initialized = true;
            }
        }
        return result;
    }

    void resetParser() { protocol.reset(); }
    void update(uint32_t now) { relay.update(now); }
    bool relayEnabled() const { return relay.isEnabled(); }
    bool isInitialized() const { return initialized; }

private:
    RelayProtocol protocol;
    RelayControl relay;
    bool initialized = false;
};
