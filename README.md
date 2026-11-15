# UART relay protocol

USART1 uses PD6 for RX and PD5 for TX at 9600 baud, 8N1. Connect the master's
TX to the first device's RX, each device's TX to the next device's RX, and the
last device's TX to the master's RX. Connect all grounds together. TX starts
disabled and is enabled when the first byte of a frame is forwarded; it stays
enabled until reset. Consequently even an invalid frame can enable TX: its
first bytes have already been forwarded before its XOR can be checked.

The MCU runs at 8 MHz from the internal oscillator divided by three. UART and
timer timings use the configured clock; measure power and verify communication
on the hardware after flashing.

## Frame format

Each command consists of exactly ten bytes:

| Byte | Meaning |
| --- | --- |
| 0 | Address |
| 1 | Command |
| 2-5 | Unsigned 32-bit parameter, big-endian |
| 6 | Command XOR: XOR of bytes 0-5 |
| 7 | Number of devices that executed the command |
| 8 | Number of devices after the last executor, or `ff` if none executed |
| 9 | Status XOR: byte 7 XOR byte 8 |

The master starts every command with status bytes `00 ff ff`. A device
validates the command XOR and, if addressed, executes the command immediately
upon receiving byte 6, without waiting for the status bytes. Each device
increments the execution count if it executed the command. An executor sets the
distance to zero; a non-executor increments it unless it is `ff`. The status
XOR is adjusted to match the changed status bytes. Unknown commands are not
executed or counted and cause a fault indication on addressed devices.

Counts and distances range from `00` to `fe`; `ff` is reserved for "no device
executed" in the distance field and is invalid as a count. The protocol supports
chains of up to 254 devices. If a counter would overflow, or an incoming status
has inconsistent fields (`count == 0` exactly when `distance == ff`), the
device signals a fault and stops forwarding the rest of that frame.

Addresses `00` through `fd` target individual devices: `00` is the first,
`01` the second, and so on through the 254th device at `fd`. A device
decrements a nonzero unicast address when forwarding. When it receives address
`00`, it forwards address `fe` instead, whether or not it recognizes the
command. Address `fe` targets no device and is forwarded unchanged, so later
devices do not execute the command. The master may also send directly to
`fe`, for example to check forwarding without executing anything. Address
`ff` is broadcast and is forwarded unchanged; every device executes a
supported broadcast command. Each device adjusts byte 6 for its address
change; the command and parameters are forwarded unchanged except for the
parameter of a read command, as described below.

For example, the master can send broadcast init:

```text
ff 02 00 00 00 00 fd 00 ff ff
```

In a chain of three devices the returned frame is
`ff 02 00 00 00 00 fd 03 00 03`. A command addressed to the second of three
devices returns count `01`, distance `01`, and status XOR `00`. A valid frame
with count `00` and distance `ff` means no device executed it, for example
when the address is beyond the end of the chain, when addressed to `fe`, or
when the addressed device does not recognize the command.

| Command | Action |
| --- | --- |
| `00` | Turn relay off |
| `ff` | Turn relay on indefinitely |
| `01` | Turn relay on for the parameter's number of milliseconds, then off |
| `02` | Initialize; leave the relay and timer unchanged |
| `03` | Restart the device |
| `04` | Disable the status LED until the device resets |
| `05` | Read relay status into the parameter |

For `05`, the master sends a zero parameter. The addressed device takes a
snapshot when it receives the command byte, and replaces the four parameter
bytes as they are forwarded, in big-endian order:

| Returned parameter | Relay status |
| --- | --- |
| `00000000` | Off |
| `ffffffff` | On indefinitely, or timed with 4294967295 ms remaining |
| `00000001`-`fffffffe` | On with that many milliseconds remaining |

The maximum timed duration is also `ffffffff` ms, so a snapshot at exactly
that remaining duration is indistinguishable from on indefinitely. A
successful read is counted once per addressed device. For broadcast, every
device overwrites the argument with its own snapshot; the returned parameter
therefore contains only the **last device's** status, not all statuses.
If the count is zero, the parameter is not a read result. A read sent to
`fe` is forwarded unchanged and does not address any device.

To keep byte-by-byte forwarding, the device emits the snapshot **before**
receiving and validating the command XOR, and adjusts the forwarded command
XOR for the changed parameter. If the incoming XOR fails, it stops forwarding
and resets as usual; the earlier parameter bytes cannot be retracted. Only
interpret a read result from a complete, validated returned frame with an
execution count of one for unicast or a nonzero count for broadcast. The
read itself does not change the relay or timer.

A timed command restarts the timer; a zero duration turns the relay off
immediately. A subsequent on or off command cancels the timer. Initialization
means TX is enabled; forwarding any frame enables it, so `02` is no longer
required to enable forwarding.

## Errors, recovery, and reset

Forwarding is byte-by-byte, not store-and-forward: the first six command bytes
have already left a device before it can check byte 6. If the command XOR is
invalid, the device does not execute the command and does not forward the
invalid XOR or the remaining status bytes. It schedules an MCU reset, discards
the rest of the frame, and resets once the frame ends or times out and any
previously queued TX bytes have finished transmitting. This applies regardless
of address; after reset the relay returns to its startup state. Previously
forwarded bytes cannot be retracted, and downstream devices discard the
incomplete frame after their timeout. The master must wait for the affected
device to restart before sending further commands. A bad status XOR does not
cause an MCU reset and is likewise not forwarded, but execution has
already happened; missing or invalid feedback does **not** mean the command was
not executed. A UART receive error or TX buffer overflow also signals a fault
and may leave a partial frame downstream.

An incomplete frame is discarded after an idle gap of three UART character
times (30 bit times, 3.125 ms at 9600 baud). The master must send each frame
without such a gap. It can pipeline commands without waiting for their
responses, but should leave at least 3.125 ms of idle time after the last
stop bit of each frame before sending the next, with additional margin for
clock and transmission timing differences. This gap lets devices downstream
discard a partial frame before the next arrives. Verify the gap at each device
when using long chains or adapters with buffering; forwarding delays can
change it.

The master must read responses concurrently if it needs feedback, track them
in order, and handle missing, delayed, or corrupt responses. There is no
sequence number, so a missing response can make later responses ambiguous,
particularly for repeated commands. Waiting for each response or timeout
before sending another is safer when reliable attribution matters. The
protocol has no delimiter or error correction: dropped or inserted bytes can
corrupt a frame, and XOR cannot detect every possible corruption.

A reset command is executed logically at its valid command XOR, but the
physical MCU reset is deferred until the frame has finished or has been
aborted, the TX queue is empty, and the final stop bit has left TX. This also
applies when status validation fails or the frame times out. The master must
wait for the entire chain to restart before sending further commands. Relays
return to their startup state on reset.

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
python3 relayctl.py --listen 1 read
python3 relayctl.py --listen ff read
python3 relayctl.py fe init
python3 relayctl.py broadcast off
python3 relayctl.py --port /dev/ttyUSB0 broadcast init
python3 relayctl.py --listen ff init
```

Every command requires an address before the command. Device addresses are
decimal `0` through `253` (leading zeroes are allowed), or hexadecimal `0x00`
through `0xfd`. The no-recipient address is `fe`, `0xfe`, `254`, or `none`;
broadcast is `ff`, `0xff`, `255`, or `broadcast`. The maximum duration is
4294967295 ms. This address format is incompatible with firmware using `fe`
as a unicast address or `05` as the delivered-command marker; update all
devices and the master together.

By default the client waits 5 ms after flushing the transmitted frame before
exiting; the printed frame confirms only a serial write, not execution. This
allows sequential invocations to leave a gap, but does not coordinate
concurrent writers. A serial driver's flush or a buffered USB adapter may not
guarantee the final stop bit has physically left the wire, so verify timing
on the hardware. With `--listen` before the address, it waits up
to two seconds for a returned frame, validates its XORs and command fields,
then prints the execution count and distance. For a successful `read`, it also
prints the relay status; for broadcast this is the last device's status.
Without `--listen`, no read result is shown. The client rejects the `fe`
read address. It exits with an error on timeout
or invalid feedback. A returned frame requires the last device's TX to be wired
to the master's RX. There is no sequence number, so stale matching feedback
cannot be distinguished from the current response.

## Status LED

Before TX is enabled, the status LED alternates on and off every second. Once
TX is enabled, the device is initialized and the LED flashes for 100 ms every
two seconds. Command `04` overrides all status LED indications, including
faults and received-frame flashes, until the device resets; it does not affect
the relay or UART forwarding. After every complete received frame, the LED
briefly switches to the opposite of its normal state for 40 ms. Frames received
during that interval restart the interval. Incomplete frames and UART receive
errors do not trigger this indication. A fault latches the LED on until reset;
faults include invalid status checksums or fields, unknown addressed commands,
incomplete-frame timeouts, UART receive errors, and transmit-buffer overflow.
An invalid command XOR also signals a fault, but the ensuing reset clears it.

## Tests

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/relay_protocol_test.cpp -o /tmp/relay_protocol_test && /tmp/relay_protocol_test
python3 -m unittest discover -s test -p 'test_relayctl.py'
```
