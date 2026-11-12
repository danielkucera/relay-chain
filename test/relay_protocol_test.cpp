#include "relay_protocol.h"
#include "status_led.h"

#include <assert.h>
#include <stdint.h>

static constexpr uint8_t InitializeFrame[] = {0, InitializeCommand, 0, 0, 0, 0, InitializeCommand};

static void checkReset()
{
    RelayNode node;
    const uint8_t local[] = {0, ResetCommand, 0, 0, 0, 0, ResetCommand};
    const uint8_t invalid[] = {0, ResetCommand, 0, 0, 0, 0, 0};
    const uint8_t broadcast[] = {0xff, ResetCommand, 0, 0, 0, 0, 0xfc};
    const uint8_t forwarded[] = {1, ResetCommand, 0, 0, 0, 0, 2};
    const uint8_t* frames[] = {invalid, local, broadcast, forwarded};
    for (const uint8_t* frame : frames) {
        for (unsigned i = 0; i < sizeof(local); ++i) {
            const NodeResult result = node.receive(frame[i], 0);
            assert(result.frameComplete == (i == 6));
            assert(result.reset == (i == 6 && (frame == local || frame == broadcast)));
            assert(result.error == (i == 6 && frame == invalid));
            assert(result.forward == (frame == broadcast || frame == forwarded));
            if (frame == broadcast) {
                assert(result.forwardByte == frame[i]);
            }
        }
    }

    ResetAfterTransmit reset;
    assert(!reset.ready(true, false, false));
    reset.request();
    assert(reset.waiting());
    assert(!reset.ready(false, true, true));
    assert(!reset.ready(true, true, false));
    assert(reset.ready(true, true, true));
    assert(reset.ready(true, false, false));
}

static void checkStatusLed()
{
    StatusLed led;
    assert(!led.update(0, false, false, false));
    assert(led.update(10, false, true, false));
    assert(led.update(49, false, false, false));
    assert(!led.update(50, false, false, false));
    assert(!led.update(999, false, false, false));
    assert(led.update(1000, false, false, false));
    assert(led.update(1999, false, false, false));
    assert(!led.update(2000, false, false, false));

    assert(led.update(2500, false, true, true));
    assert(led.update(2539, false, false, true));
    assert(!led.update(2540, false, false, true));
    assert(!led.update(4499, false, false, true));
    assert(led.update(4500, false, false, true));
    assert(led.update(4599, false, false, true));
    assert(!led.update(4600, false, false, true));
    assert(led.update(6500, false, false, true));
    assert(!led.update(6501, false, true, true));
    assert(!led.update(6540, false, false, true));
    assert(led.update(6541, false, false, true));
    assert(!led.update(6600, false, false, true));

    StatusLed fault;
    assert(!fault.update(1200, true, true, true));
    assert(!fault.update(1250, true, true, true));
    assert(!fault.update(1289, true, false, true));
    assert(fault.update(1290, true, false, true));
    assert(!fault.update(UINT32_MAX - 50, true, true, true));
    assert(!fault.update(UINT32_MAX - 11, true, false, true));
    assert(fault.update(UINT32_MAX - 10, true, false, true));
    assert(!fault.update(UINT32_MAX - 10, true, true, true));
    assert(!fault.update(28, true, false, true));
    assert(fault.update(29, true, false, true));
    led.disable();
    assert(!led.update(7000, true, true, true));
    assert(!led.update(8000, true, false, true));
    assert(!led.update(9000, false, true, true));
    StatusLed restarted;
    assert(restarted.update(0, true, false, false));

    StatusLed rollover;
    assert(!rollover.update(UINT32_MAX - 2100, false, false, true));
    assert(rollover.update(UINT32_MAX - 100, false, false, true));
    assert(rollover.update(UINT32_MAX - 1, false, false, true));
    assert(!rollover.update(UINT32_MAX, false, false, true));
    assert(!rollover.update(1898, false, false, true));
    assert(rollover.update(1899, false, false, true));
    assert(rollover.update(1998, false, false, true));
    assert(!rollover.update(1999, false, false, true));
}

static void checkStatusLedOffCommand()
{
    RelayNode node;
    StatusLed led;
    const uint8_t on[] = {0, 0xff, 0, 0, 0, 0, 0xff};
    for (uint8_t byte : on) {
        node.receive(byte, 0);
    }
    assert(node.relayEnabled());
    const uint8_t invalid[] = {0, StatusLedOffCommand, 0, 0, 0, 0, 0};
    const uint8_t local[] = {0, StatusLedOffCommand, 0, 0, 0, 0, StatusLedOffCommand};
    const uint8_t broadcast[] = {0xff, StatusLedOffCommand, 0, 0, 0, 0, 0xfb};
    const uint8_t forwarded[] = {1, StatusLedOffCommand, 0, 0, 0, 0, 5};
    const uint8_t* frames[] = {invalid, forwarded, local, broadcast};
    for (const uint8_t* frame : frames) {
        for (unsigned i = 0; i < sizeof(local); ++i) {
            const NodeResult result = node.receive(frame[i], 100);
            assert(result.disableStatusLed == (i == 6 && (frame == local || frame == broadcast)));
            assert(result.error == (i == 6 && frame == invalid));
            assert(result.forward == (frame == forwarded || frame == broadcast));
            if (frame == broadcast) {
                assert(result.forwardByte == frame[i]);
            }
            if (result.disableStatusLed) {
                led.disable();
            }
            if (frame == invalid || frame == forwarded) {
                assert(led.update(100, true, false, false));
            } else if (frame == local && i == 6) {
                assert(!led.update(100, true, true, false));
            } else if (frame == broadcast) {
                assert(!led.update(100, true, result.frameComplete, false));
            }
        }
        assert(node.relayEnabled());
    }
    const uint8_t timed[] = {0, 1, 0, 0, 0, 10, 11};
    for (uint8_t byte : timed) {
        node.receive(byte, 200);
    }
    node.update(210);
    assert(!node.relayEnabled());
    assert(!led.update(210, true, true, false));
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
            assert(!result.error && !result.enableTx);
        }
    }
    return result;
}

static void checkInitialization()
{
    RelayNode first;
    const uint8_t invalid[] = {0, 0xff, 0, 0, 0, 0, 0};
    NodeResult result = sendLocal(first, invalid, 0);
    assert(result.error && !first.relayEnabled() && !result.enableTx);

    const uint8_t invalidInit[] = {0, InitializeCommand, 0, 0, 0, 0, 0};
    result = sendLocal(first, invalidInit, 0);
    assert(result.error && !result.enableTx);

    const uint8_t unknown[] = {0, 5, 0, 0, 0, 0, 5};
    result = sendLocal(first, unknown, 0);
    assert(result.error && !first.relayEnabled());

    const uint8_t forwarded[] = {1, 0xff, 0, 0, 0, 0, 0xfe};
    for (uint8_t byte : forwarded) {
        result = first.receive(byte, 0);
        assert(result.forward && !result.error);
    }

    const uint8_t on[] = {0, 0xff, 0, 0, 0, 0, 0xff};
    result = sendLocal(first, on, 0);
    assert(!result.forward && !result.error && !result.enableTx && first.relayEnabled());
    result = sendLocal(first, InitializeFrame, 1);
    assert(!result.forward && !result.error && result.enableTx && first.relayEnabled());
    first.resetParser();
    result = sendLocal(first, unknown, 2);
    assert(result.error && !result.enableTx);

    RelayNode chain[8];
    for (RelayNode& node : chain) {
        result = sendLocal(node, InitializeFrame, 0);
        assert(!result.forward && !result.error && result.enableTx && !node.relayEnabled());
        result = sendLocal(node, InitializeFrame, 1);
        assert(!result.forward && !result.error && result.enableTx && !node.relayEnabled());
    }

    RelayNode timed;
    const uint8_t timedOn[] = {0, 1, 0, 0, 0, 10, 11};
    result = sendLocal(timed, timedOn, 10);
    assert(!result.forward && !result.error && !result.enableTx && timed.relayEnabled());
    result = sendLocal(timed, InitializeFrame, 15);
    assert(!result.forward && !result.error && result.enableTx && timed.relayEnabled());
    timed.update(20);
    assert(!timed.relayEnabled());
}

static void checkTxEnabledStatus()
{
    const uint8_t frames[][7] = {
        {1, 0xff, 0, 0, 0, 0, 0xfe},
        {0xff, 0xff, 0, 0, 0, 0, 0},
        {0, InitializeCommand, 0, 0, 0, 0, InitializeCommand},
    };
    for (unsigned frameIndex = 0; frameIndex < 3; ++frameIndex) {
        RelayNode node;
        StatusLed led;
        bool txEnabled = false;
        for (unsigned i = 0; i < 7; ++i) {
            const NodeResult result = node.receive(frames[frameIndex][i], 100);
            if (result.forward || result.enableTx) {
                txEnabled = true;
            }
            assert(txEnabled == (frameIndex < 2 || i == 6));
            led.update(100, false, result.frameComplete, txEnabled);
        }
        assert(!led.update(2099, false, false, txEnabled));
        assert(led.update(2100, false, false, txEnabled));
        assert(!led.update(2200, false, false, txEnabled));
    }

    RelayNode node;
    const uint8_t invalid[] = {0, InitializeCommand, 0, 0, 0, 0, 0};
    for (uint8_t byte : invalid) {
        const NodeResult result = node.receive(byte, 0);
        assert(!result.forward && !result.enableTx);
    }
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
            assert(result.enableTx == (i + 1 == sizeof(initialize)));
            assert(!node.relayEnabled());
            byte = result.forwardByte;
        }
    }

    const uint8_t on[] = {0xff, 0xff, 0, 0, 0, 0, 0};
    const uint8_t off[] = {0xff, 0, 0, 0, 0, 0, 0xff};
    const uint8_t timed[] = {0xff, 1, 0, 0, 0, 10, 0xf4};
    const uint8_t invalid[] = {0xff, 0, 0, 0, 0, 0, 0xfe};
    const uint8_t unknown[] = {0xff, 5, 0, 0, 0, 0, 0xfa};
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
        assert(!result.forward && !result.error && result.enableTx);
    }
}

int main()
{
    checkStatusLed();
    checkStatusLedOffCommand();
    checkReset();
    checkForwarding();
    checkInitialization();
    checkTxEnabledStatus();
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
