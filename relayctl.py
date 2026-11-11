#!/usr/bin/env python3
"""Send commands to a daisy chain of UART relay units."""

import argparse
import sys


def address_value(value):
    if value.lower() in ("broadcast", "ff"):
        return 0xff
    try:
        address = int(value, 16 if value.lower().startswith("0x") else 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use 0-254 or ff (broadcast)") from exc
    if not 0 <= address <= 255:
        raise argparse.ArgumentTypeError("use 0-254 or ff (broadcast)")
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
    checksum = 0
    for byte in frame:
        checksum ^= byte
    return frame + bytes((checksum,))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0", help="serial port (default: %(default)s)")
    parser.add_argument(
        "address", type=address_value,
        help="zero-based device address (0-254 or 0x00-0xfe); ff (0xff, 255, or broadcast) targets all devices",
    )
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("on", help="turn relay on indefinitely")
    commands.add_parser("off", help="turn relay off")
    timed = commands.add_parser("on-for", help="turn relay on for a duration")
    timed.add_argument("milliseconds", type=duration_value)
    commands.add_parser("init", help="initialize the selected device(s)")
    args = parser.parse_args(argv)

    commands_by_name = {"on": 0xff, "off": 0x00, "on-for": 0x01, "init": 0x02}
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
    except (serial.SerialException, OSError) as exc:
        print(f"Error sending command on {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Sent: {frame.hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
