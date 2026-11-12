# UART relay protocol

USART1 uses PD6 for RX at 9600 baud, 8N1. TX on PD5 is enabled when a
nonzero-address frame arrives or a valid local `init` command completes, and
remains enabled until reset. Local commands do not generate or forward frames.

The MCU runs at 8 MHz from the internal oscillator divided by three. UART and
timer timings use the configured clock; measure power and verify communication
on the hardware after flashing.

Each frame consists of exactly seven bytes:

| Byte | Meaning |
| --- | --- |
| 0 | Address |
| 1 | Command |
| 2-5 | Unsigned 32-bit parameter, big-endian |
| 6 | XOR of bytes 0-5 |

Unicast frames with an address from `01` to `fe` are forwarded without local
execution or checksum validation. Each device decrements the address by one and adjusts the checksum
as `old_checksum XOR old_address XOR new_address`; all other bytes are unchanged.
An invalid checksum remains invalid. Address zero targets the first device,
address one the second, and so on. Frames received with address zero are not
forwarded and are executed only if their checksum is valid:

Address `ff` is reserved for broadcast: every device forwards each byte unchanged
as it arrives and executes the command only after verifying the complete frame's
checksum. Invalid broadcasts are still forwarded but do not change the relay.
For example, `ff ff 00 00 00 00 00` turns every relay on.

| Command | Action |
| --- | --- |
| `00` | Turn relay off |
| `ff` | Turn relay on indefinitely |
| `01` | Turn relay on for the parameter's number of milliseconds, then off |
| `02` | Enable TX; leave the relay and timer unchanged |
| `03` | Restart the device |
| `04` | Disable the status LED until the device resets |

A timed command restarts the timer; a zero duration turns the relay off
immediately. A subsequent on or off command cancels the timer. Unknown commands
and local frames with invalid checksums leave the relay unchanged.

A valid reset restarts the MCU, returning the relay and status to their startup
states. A device receiving a broadcast reset first sends all queued bytes and
waits for the final stop bit to leave TX before restarting. A local reset also
waits for any previously queued transmission. The master must wait for the
entire chain to restart and reinitialize it before sending further commands;
the protocol provides no acknowledgment or flow control.

To initialize the chain, the master can send the broadcast frame
`ff 02 00 00 00 00 fd`. Each device enables TX upon receiving the address and
forwards each byte as it arrives. Devices do not generate initialization
frames of their own.

## Command-line control

Install the Python serial library with `python3 -m pip install pyserial` (or
install your distribution's `python3-serial` package). The client uses
`/dev/ttyACM0` at 9600 baud, 8N1 by default:

```sh
python3 relayctl.py ff init
python3 relayctl.py ff reset
python3 relayctl.py ff led-off
python3 relayctl.py 0 on
python3 relayctl.py 1 off
python3 relayctl.py 02 on-for 5000
python3 relayctl.py broadcast off
python3 relayctl.py --port /dev/ttyUSB0 broadcast init
```

Every command requires an address before the command. Device addresses are
decimal `0` through `254` (leading zeroes are allowed), or hexadecimal `0x00`
through `0xfe`; broadcast is `ff`, `0xff`, `255`, or `broadcast`.
A zero duration turns the relay off immediately,
and the maximum duration is 4294967295 ms. The printed frame confirms only
that it was written to the serial port: the protocol has no acknowledgment.
After sending, the client stays open and prints received bytes in hexadecimal
until you press Ctrl+C. Receiving the forwarded frame requires the far end of
the chain to be connected to the serial adapter's RX pin and a common ground.

Run the client tests on Linux with `python3 -m unittest discover -s test -p 'test_relayctl.py'`.

Before TX is enabled, the status LED alternates on and off every second. Once
TX is enabled, the device is initialized and the LED flashes for 100 ms every
two seconds. TX is enabled by any frame that must be forwarded, even before its
checksum is received, or by a valid local `init` command.
Command `04` overrides all status LED indications,
including faults and received-frame flashes, until the device resets; it does
not affect the relay or UART forwarding. After every complete received frame, including forwarded
and invalid frames, the LED briefly switches to the opposite of its normal
state for 40 ms. Frames received during that interval restart the interval.
Incomplete frames and UART receive errors do not trigger this indication.
It also stays on after an invalid local frame, unknown command, UART receive
error, or transmit-buffer overflow, until reset. A receive error discards the affected byte and
resets the frame parser.
Because the protocol has no delimiter, length marker, or escape sequence, a
lost or extra byte can leave the stream out of alignment; resetting the device
or otherwise re-establishing a frame boundary may be necessary.

The host-side protocol tests can be run with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/relay_protocol_test.cpp -o /tmp/relay_protocol_test && /tmp/relay_protocol_test
```
