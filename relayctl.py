#!/usr/bin/env python3
"""Send a UART relay command and optionally wait for chain feedback."""

import argparse
import sys
import time

INTER_FRAME_GAP_SECONDS = 0.005
READ_COMMAND = 0x05


def xor_bytes(data):
    checksum = 0
    for byte in data:
        checksum ^= byte
    return checksum


def address_value(value):
    if value.lower() in ("broadcast", "ff"):
        return 0xff
    if value.lower() in ("none", "fe"):
        return 0xfe
    try:
        address = int(value, 16 if value.lower().startswith("0x") else 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use 0-253, fe (none), or ff (broadcast)") from exc
    if not 0 <= address <= 255:
        raise argparse.ArgumentTypeError("use 0-253, fe (none), or ff (broadcast)")
    return address


def duration_value(value):
    try:
        duration = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("duration must be an integer in milliseconds") from exc
    if not 0 <= duration <= 0xffffffff:
        raise argparse.ArgumentTypeError("duration must be between 0 and 4294967295 ms")
    return duration


def make_frame(address, command, parameter=0):
    frame = bytes((address, command)) + parameter.to_bytes(4, "big")
    return frame + bytes((xor_bytes(frame), 0, 0xff, 0xff))


def parse_feedback(frame, sent):
    if len(frame) != 10:
        raise ValueError(f"incomplete response ({len(frame)} of 10 bytes)")
    if xor_bytes(frame[:7]) != 0:
        raise ValueError("invalid command XOR")
    if xor_bytes(frame[7:]) != 0:
        raise ValueError("invalid status XOR")
    count, distance = frame[7:9]
    if count == 0xff or (count == 0) != (distance == 0xff):
        raise ValueError("invalid status fields")
    is_read_result = (sent[1] == READ_COMMAND and
                      ((sent[0] < 0xfe and count == 1) or
                       (sent[0] == 0xff and count > 0)))
    if not is_read_result and frame[2:6] != sent[2:6]:
        raise ValueError("response does not match the command")
    if sent[0] == 0xff:
        matches = (frame[0] == sent[0] and frame[1] == sent[1] and count > 0)
    elif sent[0] == 0xfe:
        matches = frame[:7] == sent[:7] and count == 0
    else:
        matches = (frame[1] == sent[1] and
                   ((frame[0] == 0xfe and count <= 1) or
                    (count == 0 and frame[0] < sent[0])))
    if not matches:
        raise ValueError("response does not match the command")
    return count, distance


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0", help="serial port (default: %(default)s)")
    parser.add_argument("--listen", action="store_true",
                        help="wait up to 2 seconds for the returned frame and display feedback")
    parser.add_argument(
        "address", type=address_value,
        help="device 0-253 (0x00-0xfd); fe (0xfe, 254, or none) targets none; ff targets all",
    )
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("on", help="turn relay on indefinitely")
    commands.add_parser("off", help="turn relay off")
    timed = commands.add_parser("on-for", help="turn relay on for a duration")
    timed.add_argument("milliseconds", type=duration_value)
    commands.add_parser("init", help="initialize the selected device(s)")
    commands.add_parser("reset", help="restart the selected device(s)")
    commands.add_parser("led-off", help="disable status LED until reset")
    commands.add_parser("read", help="read relay status; broadcast returns the last device's status")
    args = parser.parse_args(argv)
    if args.action == "read" and args.address == 0xfe:
        parser.error("read requires a unicast or broadcast address")

    commands_by_name = {
        "on": 0xff, "off": 0x00, "on-for": 0x01, "init": 0x02,
        "reset": 0x03, "led-off": 0x04,
        "read": READ_COMMAND,
    }
    duration = args.milliseconds if args.action == "on-for" else 0
    frame = make_frame(args.address, commands_by_name[args.action], duration)

    try:
        import serial
    except ImportError:
        print("Error: pyserial is required; install it with: python3 -m pip install pyserial",
              file=sys.stderr)
        return 1

    try:
        with serial.Serial(
            args.port, baudrate=9600, bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
            write_timeout=1,
        ) as uart:
            written = uart.write(frame)
            if written != len(frame):
                raise serial.SerialException(f"only {written} of {len(frame)} bytes written")
            uart.flush()
            if not args.listen:
                time.sleep(INTER_FRAME_GAP_SECONDS)
            print(f"Sent: {frame.hex(' ')}", flush=True)
            if args.listen:
                deadline = time.monotonic() + 2
                received = bytearray()
                while len(received) < 10:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    uart.timeout = remaining
                    chunk = uart.read(10 - len(received))
                    if not chunk:
                        break
                    received.extend(chunk)
                print(f"Received: {received.hex(' ')}", flush=True)
                try:
                    count, distance = parse_feedback(received, frame)
                except ValueError as exc:
                    print(f"Feedback error: {exc}", file=sys.stderr)
                    return 1
                if count:
                    print(f"Executed: {count}; devices after last executor: {distance}")
                    if args.action == "read":
                        value = int.from_bytes(received[2:6], "big")
                        if args.address == 0xff:
                            print("Status from last device:")
                        if value == 0:
                            print("Relay: off")
                        elif value == 0xffffffff:
                            print("Relay: on indefinitely (or timed with 4294967295 ms remaining)")
                        else:
                            print(f"Relay: on; {value} ms remaining")
                else:
                    print("Executed: 0; no device executed the command")
    except KeyboardInterrupt:
        print("\nStopped listening.")
        return 0
    except (serial.SerialException, OSError) as exc:
        print(f"Serial error on {args.port}: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
