#include "relay_protocol.h"
#include "status_led.h"

#include <assert.h>
#include <stdint.h>

static constexpr uint8_t InitializeFrame[] = {0, InitializeCommand, 0, 0, 0, 0, InitializeCommand};

static void checkStatusLed()
{
    StatusLed led;
    assert(!led.update(0, false, false));
    assert(led.update(10, false, true));
    assert(led.update(109, false, false));
    assert(!led.update(110, false, false));
    assert(led.update(1000, false, false));
    assert(!led.update(1001, false, true));
    assert(!led.update(1100, false, false));
    assert(led.update(1101, false, false));
    assert(!led.update(1200, true, true));
    assert(!led.update(1250, true, true));
    assert(!led.update(1349, true, false));
    assert(led.update(1350, true, false));
    assert(!led.update(UINT32_MAX - 50, true, true));
    assert(!led.update(48, true, false));
    assert(led.update(49, true, false));
}

static void checkForwarding()
{
    for (unsigned address = 1; address < BroadcastAddress; ++address) {
        for (unsigned corrupt = 0; corrupt <= 1; ++corrupt) {
            const uint8_t input[] = {
                static_cast<uint8_t>(address), 1, 0x12, 0x34, 0x56, 0x78,
                static_cast<uint8_t>(address ^ 1 ^ 0x12 ^ 0x34 ^ 0x56 ^ 0x78 ^ corrupt)
            };
            uint8_t output[sizeof(input)] = {};
            RelayProtocol first;
            for (unsigned i = 0; i < sizeof(input); ++i) {
                const ProtocolResult result = first.receive(input[i]);
                assert(result.forward && !result.localComplete);
                assert(result.frameComplete == (i == 6));
                output[i] = result.forwardByte;
                if (i == 0) {
                    assert(output[i] == address - 1);
                } else if (i < 6) {
                    assert(output[i] == input[i]);
                } else {
                    assert(output[i] == (input[i] ^ address ^ (address - 1)));
                }
            }

            RelayProtocol second;
            for (unsigned i = 0; i < sizeof(output); ++i) {
                const ProtocolResult result = second.receive(output[i]);
                if (address == 1) {
                    assert(!result.forward);
                    if (i == 6) {
                        assert(result.localComplete);
                        assert(result.checksumValid == (corrupt == 0));
                        assert(result.command == 1);
                        assert(result.parameter == 0x12345678);
                    }
                } else {
                    assert(result.forward && !result.localComplete);
                    if (i == 0) {
                        assert(result.forwardByte == address - 2);
                    } else if (i == 6) {
                        assert(result.forwardByte == (input[i] ^ address ^ (address - 2)));
                    } else {
                        assert(result.forwardByte == input[i]);
                    }
                }
            }
        }
    }
}

static void checkDaisyChain(uint8_t address, bool corrupt)
{
    uint8_t frame[] = {address, 0xff, 0, 0, 0, 0,
                       static_cast<uint8_t>(address ^ 0xff ^ corrupt)};
    for (unsigned hop = 0; hop <= address; ++hop) {
        RelayProtocol device;
        RelayControl relay;
        for (unsigned i = 0; i < sizeof(frame); ++i) {
            const ProtocolResult result = device.receive(frame[i]);
            if (hop < address) {
                assert(result.forward && !result.localComplete);
                frame[i] = result.forwardByte;
            } else {
                assert(!result.forward);
                if (result.localComplete && result.checksumValid) {
                    assert(relay.command(result.command, result.parameter, 0));
                }
            }
        }
        assert(relay.isEnabled() == (hop == address && !corrupt));
    }
}

static NodeResult sendLocal(RelayNode& node, const uint8_t* frame, uint32_t now)
{
    NodeResult result = {};
    for (unsigned i = 0; i < sizeof(InitializeFrame); ++i) {
        result = node.receive(frame[i], now);
        assert(!result.forward);
        assert(result.frameComplete == (i + 1 == sizeof(InitializeFrame)));
        if (i + 1 < sizeof(InitializeFrame)) {
            assert(!result.error);
        }
    }
    return result;
}

static void checkInitialization()
{
    RelayNode first;
    assert(!first.isInitialized());
    const uint8_t invalid[] = {0, 0xff, 0, 0, 0, 0, 0};
    NodeResult result = sendLocal(first, invalid, 0);
    assert(result.error && !first.relayEnabled() && !first.isInitialized());

    const uint8_t invalidInit[] = {0, InitializeCommand, 0, 0, 0, 0, 0};
    result = sendLocal(first, invalidInit, 0);
    assert(result.error && !first.isInitialized());

    const uint8_t unknown[] = {0, 3, 0, 0, 0, 0, 3};
    result = sendLocal(first, unknown, 0);
    assert(result.error && !first.relayEnabled());

    const uint8_t forwarded[] = {1, 0xff, 0, 0, 0, 0, 0xfe};
    for (uint8_t byte : forwarded) {
        result = first.receive(byte, 0);
        assert(result.forward && !result.error);
    }

    const uint8_t on[] = {0, 0xff, 0, 0, 0, 0, 0xff};
    result = sendLocal(first, on, 0);
    assert(!result.forward && !result.error && first.relayEnabled() && !first.isInitialized());
    result = sendLocal(first, InitializeFrame, 1);
    assert(!result.forward && !result.error && first.relayEnabled() && first.isInitialized());
    first.resetParser();
    result = sendLocal(first, unknown, 2);
    assert(result.error && first.isInitialized());

    RelayNode chain[8];
    for (RelayNode& node : chain) {
        result = sendLocal(node, InitializeFrame, 0);
        assert(!result.forward && !result.error && !node.relayEnabled());
        result = sendLocal(node, InitializeFrame, 1);
        assert(!result.forward && !result.error && !node.relayEnabled());
    }

    RelayNode timed;
    const uint8_t timedOn[] = {0, 1, 0, 0, 0, 10, 11};
    result = sendLocal(timed, timedOn, 10);
    assert(!result.forward && !result.error && timed.relayEnabled());
    result = sendLocal(timed, InitializeFrame, 15);
    assert(!result.forward && !result.error && timed.relayEnabled());
    timed.update(20);
    assert(!timed.relayEnabled());
}

static void checkBroadcast()
{
    RelayNode chain[8];
    uint8_t initialize[] = {0xff, InitializeCommand, 0, 0, 0, 0, 0xfd};
    for (RelayNode& node : chain) {
        for (unsigned i = 0; i < sizeof(initialize); ++i) {
            uint8_t& byte = initialize[i];
            const NodeResult result = node.receive(byte, 0);
            assert(result.forward && result.forwardByte == byte && !result.error);
            assert(result.frameComplete == (i + 1 == sizeof(initialize)));
            assert(!node.relayEnabled());
            byte = result.forwardByte;
        }
        assert(node.isInitialized());
    }

    const uint8_t on[] = {0xff, 0xff, 0, 0, 0, 0, 0};
    const uint8_t off[] = {0xff, 0, 0, 0, 0, 0, 0xff};
    const uint8_t timed[] = {0xff, 1, 0, 0, 0, 10, 0xf4};
    const uint8_t invalid[] = {0xff, 0, 0, 0, 0, 0, 0xfe};
    const uint8_t unknown[] = {0xff, 3, 0, 0, 0, 0, 0xfc};
    const uint8_t* frames[] = {on, invalid, unknown, off, timed};

    for (const uint8_t* frame : frames) {
        for (RelayNode& node : chain) {
            for (unsigned i = 0; i < sizeof(on); ++i) {
                const NodeResult result = node.receive(frame[i], 100);
                assert(result.forward && result.forwardByte == frame[i]);
                assert(result.frameComplete == (i == 6));
                assert(result.error == (i == 6 && (frame == invalid || frame == unknown)));
                if (i < 6) {
                    if (frame == invalid || frame == unknown || frame == off) {
                        assert(node.relayEnabled());
                    } else {
                        assert(!node.relayEnabled());
                    }
                }
            }
            assert(node.relayEnabled() == (frame != off));
        }
    }

    for (RelayNode& node : chain) {
        node.update(109);
        assert(node.relayEnabled());
        node.update(110);
        assert(!node.relayEnabled());
        NodeResult result = sendLocal(node, InitializeFrame, 111);
        assert(!result.forward && !result.error);
    }
}

int main()
{
    checkStatusLed();
    checkForwarding();
    checkInitialization();
    checkBroadcast();
    for (unsigned address = 0; address < BroadcastAddress; ++address) {
        checkDaisyChain(static_cast<uint8_t>(address), false);
        checkDaisyChain(static_cast<uint8_t>(address), true);
    }
    RelayProtocol protocol;
    const uint8_t local[] = {0, 1, 0x12, 0x34, 0x56, 0x78, 0x09};
    for (unsigned i = 0; i < sizeof(local); ++i) {
        const ProtocolResult result = protocol.receive(local[i]);
        assert(!result.forward);
        assert(result.localComplete == (i == 6));
        if (result.localComplete) {
            assert(result.checksumValid);
            assert(result.command == 1);
            assert(result.parameter == 0x12345678);
        }
    }

    const uint8_t forwarded[] = {2, 0xff, 0, 0, 0, 0, 0};
    for (uint8_t byte : forwarded) {
        const ProtocolResult result = protocol.receive(byte);
        assert(result.forward);
        assert(!result.localComplete);
    }
    for (unsigned i = 0; i < sizeof(local); ++i) {
        const ProtocolResult result = protocol.receive(i == 6 ? 0 : local[i]);
        if (i == 6) {
            assert(result.localComplete && !result.checksumValid);
        }
    }

    protocol.receive(42);
    protocol.reset();
    for (unsigned i = 0; i < sizeof(local); ++i) {
        const ProtocolResult result = protocol.receive(local[i]);
        assert(!result.forward);
        if (i == 6) {
            assert(result.localComplete && result.checksumValid);
        }
    }

    RelayControl relay;
    assert(!relay.isEnabled());
    assert(relay.command(0xff, 0, 0));
    assert(relay.isEnabled());
    relay.update(100);
    assert(relay.isEnabled());
    assert(!relay.command(2, 0, 100));
    assert(relay.isEnabled());
    assert(relay.command(0, 0, 100));
    assert(!relay.isEnabled());
    assert(relay.command(1, 0, 100));
    assert(!relay.isEnabled());

    assert(relay.command(1, 20, UINT32_MAX - 10));
    relay.update(UINT32_MAX - 1);
    assert(relay.isEnabled());
    relay.update(8);
    assert(relay.isEnabled());
    relay.update(9);
    assert(!relay.isEnabled());

    assert(relay.command(1, UINT32_MAX, 0));
    relay.update(UINT32_MAX - 2);
    assert(relay.isEnabled());
    relay.update(1);
    assert(!relay.isEnabled());

    assert(relay.command(1, 10, 0));
    relay.update(5);
    assert(relay.command(1, 10, 5));
    relay.update(10);
    assert(relay.isEnabled());
    relay.update(15);
    assert(!relay.isEnabled());

    assert(relay.command(1, 10, 20));
    assert(relay.command(0xff, 0, 21));
    relay.update(31);
    assert(relay.isEnabled());
    assert(relay.command(0, 0, 32));
    assert(!relay.isEnabled());
}
