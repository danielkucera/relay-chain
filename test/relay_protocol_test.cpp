#include "relay_protocol.h"
#include "status_led.h"

#include <assert.h>
#include <stdint.h>
#include <vector>

using Bytes = std::vector<uint8_t>;

static Bytes frame(uint8_t address, uint8_t command, uint32_t parameter = 0)
{
    Bytes bytes = {address, command, static_cast<uint8_t>(parameter >> 24),
                   static_cast<uint8_t>(parameter >> 16),
                   static_cast<uint8_t>(parameter >> 8), static_cast<uint8_t>(parameter)};
    uint8_t checksum = 0;
    for (uint8_t byte : bytes) checksum ^= byte;
    bytes.insert(bytes.end(), {checksum, 0, 0xff, 0xff});
    return bytes;
}

static Bytes send(RelayNode& node, const Bytes& input, bool* error = nullptr,
                  unsigned* resets = nullptr, unsigned* ledOff = nullptr)
{
    Bytes output;
    for (uint8_t byte : input) {
        const NodeResult result = node.receive(byte, 100);
        if (result.forward) output.push_back(result.forwardByte);
        if (error && result.error) *error = true;
        if (resets && result.reset) ++*resets;
        if (ledOff && result.disableStatusLed) ++*ledOff;
    }
    return output;
}

static Bytes chain(RelayNode* nodes, unsigned length, Bytes bytes)
{
    for (unsigned i = 0; i < length; ++i) {
        bool error = false;
        bytes = send(nodes[i], bytes, &error);
        assert(!error);
    }
    return bytes;
}

static void checkChain()
{
    RelayNode nodes[254];
    for (unsigned address = 0; address < 254; ++address) {
        const Bytes output = chain(nodes, 254, frame(address, 0xff));
        assert(output.size() == 10);
        assert(output[0] == NoRecipientAddress && output[1] == 0xff);
        assert(output[6] == (NoRecipientAddress ^ 0xff));
        assert(output[7] == 1 && output[8] == 253 - address);
        assert(output[9] == (output[7] ^ output[8]));
        for (unsigned i = 0; i < 254; ++i) assert(nodes[i].relayEnabled() == (i <= address));
    }
    Bytes output = chain(nodes, 254, frame(0xff, 0));
    assert(output[0] == 0xff && output[1] == 0);
    assert(output[7] == 254 && output[8] == 0 && output[9] == 254);
    for (const RelayNode& node : nodes) assert(!node.relayEnabled());
    output = chain(nodes, 254, frame(254, 0xff));
    assert(output[0] == NoRecipientAddress && output[1] == 0xff);
    assert(output[7] == 0 && output[8] == 0xff && output[9] == 0xff);

    RelayNode timed[4];
    output = chain(timed, 4, frame(1, 1, 123));
    assert(output[7] == 1 && output[8] == 2);
    assert(timed[1].relayEnabled());
    timed[1].update(222);
    assert(timed[1].relayEnabled());
    timed[1].update(223);
    assert(!timed[1].relayEnabled());
}

static void checkErrors()
{
    RelayNode node;
    Bytes bad = frame(3, 0xff);
    bad[6] ^= 1;
    bool error = false;
    unsigned resets = 0;
    Bytes output = send(node, bad, &error, &resets);
    assert(error && resets == 1 && output.size() == 6 && !node.relayEnabled());
    error = false;
    output = send(node, frame(0, 0xff), &error);
    assert(!error && output.size() == 10 && node.relayEnabled());

    bad = frame(0, 0);
    bad[9] ^= 1;
    error = false;
    resets = 0;
    output = send(node, bad, &error, &resets);
    assert(error && resets == 0 && output.size() == 9 && !node.relayEnabled());

    bad = frame(0, 0x7f);
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 10 && output[7] == 0 && output[8] == 0xff);

    bad = frame(0, 0xff);
    bad[7] = 254;
    bad[8] = 0;
    bad[9] = 254;
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 7 && node.relayEnabled());

    bad = frame(1, 0xff);
    bad[7] = 1;
    bad[8] = 254;
    bad[9] = 255;
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 8);

    bad = frame(0, 0xff);
    bad[8] = 0;
    bad[9] = 0;
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 8);

    bad = frame(0xff, 0);
    bad[9] ^= 1;
    RelayNode downstream;
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 9 && !node.relayEnabled());
    send(downstream, output);
    assert(downstream.receiving() && !downstream.relayEnabled());
    assert(!downstream.abort());

    bad = frame(0xff, 0x7f);
    error = false;
    output = send(node, bad, &error);
    assert(error && output.size() == 10 && output[7] == 0 && output[8] == 0xff);
}

static void checkStreaming()
{
    RelayNode nodes[3];
    const Bytes input = frame(1, 0xff);
    Bytes output;
    for (unsigned i = 0; i < input.size(); ++i) {
        uint8_t byte = input[i];
        for (RelayNode& node : nodes) {
            const NodeResult result = node.receive(byte, 100);
            assert(result.forward && !result.error);
            byte = result.forwardByte;
        }
        output.push_back(byte);
        assert(nodes[1].relayEnabled() == (i >= 6));
        assert(!nodes[0].relayEnabled() && !nodes[2].relayEnabled());
    }
    assert(output[7] == 1 && output[8] == 1 && output[9] == 0);

    Bytes corrupt = frame(0xff, 0);
    corrupt[6] ^= 1;
    Bytes prefix = send(nodes[0], corrupt);
    assert(prefix.size() == 6);
    send(nodes[1], prefix);
    assert(nodes[1].receiving());
    assert(!nodes[1].abort());
    const Bytes next = chain(nodes, 3, frame(0xff, 0));
    assert(next[7] == 3 && next[8] == 0 && !nodes[1].relayEnabled());
}

static void checkNoRecipientAddress()
{
    RelayNode nodes[3];
    for (uint8_t command : {uint8_t(0), uint8_t(0xff), uint8_t(1),
                            InitializeCommand, ResetCommand, StatusLedOffCommand,
                            uint8_t(0x05), uint8_t(0x7f)}) {
        Bytes input = frame(NoRecipientAddress, command, 123);
        for (RelayNode& node : nodes) {
            bool error = false;
            unsigned resets = 0;
            unsigned ledOff = 0;
            Bytes output = send(node, input, &error, &resets, &ledOff);
            assert(!error && resets == 0 && ledOff == 0);
            assert(output == input && !node.relayEnabled());
            input = output;
        }
    }

    Bytes unknown = frame(1, 0x05, 123);
    bool error = false;
    Bytes first = send(nodes[0], unknown, &error);
    assert(!error && first[0] == 0 && first[1] == 0x05);
    Bytes second = send(nodes[1], first, &error);
    assert(error && second[0] == NoRecipientAddress && second[1] == 0x05);
    assert(second[6] == (NoRecipientAddress ^ 0x05 ^ 123));
    error = false;
    Bytes third = send(nodes[2], second, &error);
    assert(!error && third == second && third[7] == 0 && third[8] == 0xff);

    Bytes invalid = frame(NoRecipientAddress, 0xff);
    invalid[6] ^= 1;
    unsigned resets = 0;
    assert(send(nodes[0], invalid, &error, &resets).size() == 6);
    assert(error && resets == 1 && !nodes[0].relayEnabled());
}

static void checkResetAndTimeout()
{
    RelayNode node;
    Bytes reset = frame(0, ResetCommand);
    for (unsigned i = 0; i < 9; ++i) {
        const NodeResult result = node.receive(reset[i], 0);
        assert(!result.reset && result.forward);
    }
    const NodeResult last = node.receive(reset[9], 0);
    assert(last.reset && last.forward && last.frameComplete);
    for (unsigned i = 0; i < 7; ++i) node.receive(reset[i], 0);
    assert(node.receiving());
    assert(node.abort() && !node.receiving());
    node.receive(0, 0);
    assert(!node.abort());
    unsigned resets = 0;
    send(node, frame(0xff, ResetCommand), nullptr, &resets);
    assert(resets == 1);
    Bytes badReset = frame(0, ResetCommand);
    badReset[9] ^= 1;
    bool error = false;
    resets = 0;
    assert(send(node, badReset, &error, &resets).size() == 9);
    assert(error && resets == 1);
    unsigned ledOff = 0;
    send(node, frame(0, StatusLedOffCommand), nullptr, nullptr, &ledOff);
    assert(ledOff == 1);

    FrameTimeout timeout(3125);
    timeout.received(UINT32_MAX - 1000);
    assert(!timeout.expired(2123));
    assert(timeout.expired(2124));

    ResetAfterTransmit pending;
    assert(!pending.ready(true, true, true));
    pending.request();
    assert(!pending.ready(false, true, true));
    assert(!pending.ready(true, true, false));
    assert(pending.ready(true, true, true));

    for (uint8_t address : {uint8_t(0), uint8_t(3), BroadcastAddress}) {
        RelayNode invalid;
        const Bytes turnOn = frame(0, 0xff);
        send(invalid, turnOn);
        assert(invalid.relayEnabled());
        Bytes bad = frame(address, 0);
        bad[6] ^= 1;
        for (unsigned i = 0; i < 7; ++i) {
            const NodeResult result = invalid.receive(bad[i], 0);
            assert(result.forward == (i < 6));
            assert(result.error == (i == 6));
            assert(!result.reset && !result.frameComplete);
        }
        assert(invalid.relayEnabled());
        assert(invalid.abort());
        assert(!invalid.receiving());
    }
}

static void checkLedAndRelay()
{
    StatusLed led;
    assert(!led.update(0, false, false, false));
    assert(led.update(1000, false, false, false));
    assert(!led.update(2000, false, false, false));
    assert(!led.update(2500, false, false, true));
    assert(led.update(4500, false, false, true));
    assert(!led.update(4600, false, false, true));
    assert(led.update(4700, false, true, true));
    assert(!led.update(4740, false, false, true));
    assert(led.update(4800, true, false, true));
    led.disable();
    assert(!led.update(5000, true, true, true));

    RelayControl relay;
    assert(relay.command(1, 20, UINT32_MAX - 10));
    relay.update(8);
    assert(relay.isEnabled());
    relay.update(9);
    assert(!relay.isEnabled());
    assert(relay.command(1, 0, 10) && !relay.isEnabled());
    assert(relay.command(0xff, 0, 10) && relay.isEnabled());
    assert(!relay.command(0x7f, 0, 10) && relay.isEnabled());
    assert(relay.command(0, 0, 10) && !relay.isEnabled());
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
}

int main()
{
    checkChain();
    checkErrors();
    checkStreaming();
    checkNoRecipientAddress();
    checkResetAndTimeout();
    checkLedAndRelay();
}
