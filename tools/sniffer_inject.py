#!/usr/bin/env python3
"""
tools/sniffer_inject.py — OTA frame injection via sniffer board.

Sends commands to the sniffer ESP32 (running components/sniffer/sniffer.c
with UART command support) to inject raw 802.11 frames over the air.

Commands supported by the sniffer firmware:
  INJECT <hex_payload>        — Transmit raw frame verbatim
  BITFLIP <hex> <bit_index>   — Flip one bit at bit_index, then transmit
  BURST <hex> <start> <num>   — Flip num_bits starting at start_bit, then transmit
  STATUS                      — Report sniffer state
  STOP                        — Stop the sniffer

Usage:
  # Replay a captured frame verbatim
  python tools/sniffer_inject.py --port COM6 --hex-file logs/A1_frame.hex

  # Inject with a 1-bit error at bit position 42
  python tools/sniffer_inject.py --port COM6 --hex-file logs/D_clean.log --bit-flip 42

  # Inject with a burst error (5 bits starting at bit 100)
  python tools/sniffer_inject.py --port COM6 --hex-file logs/D_clean.log --burst 100 5

  # Interactive mode (send commands one at a time)
  python tools/sniffer_inject.py --port COM6 --interactive
"""

from __future__ import annotations

import argparse
import serial
import sys
import time
from typing import Optional, List


def open_serial(port: str, baud: int = 921600, timeout: float = 2.0) -> serial.Serial:
    """Open a serial connection to the sniffer board."""
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.dtr = False
        ser.rts = False
        ser.timeout = timeout
        ser.open()
        time.sleep(1.0)  # Let the board settle
        ser.reset_input_buffer()
        return ser
    except serial.SerialException as e:
        print(f"[ERROR] Could not open serial port {port}: {e}", file=sys.stderr)
        sys.exit(1)


def send_command(ser: serial.Serial, cmd: str, wait_for_ack: bool = True) -> str:
    """Send a command to the sniffer and optionally wait for a response."""
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()

    if not wait_for_ack:
        return ""

    # Read response lines until we see a result or timeout
    deadline = time.time() + ser.timeout
    lines: List[str] = []
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode("utf-8", errors="replace").rstrip("\n\r")
            if line:
                lines.append(line)
                # Heuristic: stop when we see a result line
                if any(k in line.upper() for k in ("OK", "FAILED", "ERROR", "STATUS:", "EXITING")):
                    break
        time.sleep(0.01)

    return "\n".join(lines)


def inject_hex_file(ser: serial.Serial, hex_file: str, delay: float = 0.5) -> int:
    """Inject each hex line from a file as a separate frame."""
    injected = 0
    with open(hex_file, "r") as f:
        for line in f:
            hex_str = line.strip()
            if not hex_str:
                continue
            resp = send_command(ser, f"INJECT {hex_str}")
            print(f"  INJECT ({len(hex_str)//2} bytes): {resp.strip() or '(no response)'}")
            injected += 1
            if delay > 0:
                time.sleep(delay)
    return injected


def inject_bitflip(ser: serial.Serial, hex_file: str, bit_index: int,
                   count: int = 1, delay: float = 0.5) -> int:
    """Inject frames with a bit flipped at bit_index."""
    injected = 0
    with open(hex_file, "r") as f:
        lines = [l.strip() for l in f if l.strip()]

    for i in range(count):
        hex_str = lines[i % len(lines)]
        resp = send_command(ser, f"BITFLIP {hex_str} {bit_index}")
        print(f"  BITFLIP bit={bit_index} ({len(hex_str)//2} bytes): {resp.strip() or '(no response)'}")
        injected += 1
        if delay > 0:
            time.sleep(delay)
    return injected


def inject_burst(ser: serial.Serial, hex_file: str, start_bit: int, num_bits: int,
                 count: int = 1, delay: float = 0.5) -> int:
    """Inject frames with a burst error."""
    injected = 0
    with open(hex_file, "r") as f:
        lines = [l.strip() for l in f if l.strip()]

    for i in range(count):
        hex_str = lines[i % len(lines)]
        resp = send_command(ser, f"BURST {hex_str} {start_bit} {num_bits}")
        print(f"  BURST start={start_bit} num={num_bits} ({len(hex_str)//2} bytes): {resp.strip() or '(no response)'}")
        injected += 1
        if delay > 0:
            time.sleep(delay)
    return injected


def interactive_mode(ser: serial.Serial) -> None:
    """Interactive command mode."""
    print("=== Sniffer Interactive Mode ===")
    print("Commands: INJECT <hex> | BITFLIP <hex> <bit> | BURST <hex> <start> <num>")
    print("          STATUS | STOP | quit")
    print("Type 'quit' to exit.\n")

    while True:
        try:
            cmd = input("sniffer> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nExiting.")
            break

        if not cmd:
            continue
        if cmd.lower() in ("quit", "exit", "q"):
            break

        resp = send_command(ser, cmd)
        print(resp.strip() or "(no response)")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="OTA frame injection via CEE-PEW sniffer board."
    )
    parser.add_argument("--port", default="COM6", help="Serial port of sniffer board (frame output)")
    parser.add_argument("--cmd-port", default=None,
                        help="Serial port for command input (default: same as --port)")
    parser.add_argument("--baud", type=int, default=921600, help="UART baud rate")
    parser.add_argument("--hex-file", help="File with hex payloads (one per line)")
    parser.add_argument("--bit-flip", type=int, metavar="BIT",
                        help="Flip bit at position BIT before injecting")
    parser.add_argument("--burst", nargs=2, type=int, metavar=("START", "NUM"),
                        help="Flip NUM bits starting at START before injecting")
    parser.add_argument("--count", type=int, default=1,
                        help="Number of times to inject each frame (default: 1)")
    parser.add_argument("--delay", type=float, default=0.5,
                        help="Delay between injections in seconds (default: 0.5)")
    parser.add_argument("--interactive", action="store_true",
                        help="Interactive command mode")
    args = parser.parse_args(argv)

    # Determine which serial port to use for commands. By default, use the same port as output.
    cmd_port = args.cmd_port if getattr(args, 'cmd_port', None) else args.port

    ser = open_serial(cmd_port, args.baud)

    try:
        if args.interactive:
            interactive_mode(ser)
            return 0

        if args.hex_file:
            if args.bit_flip is not None:
                injected = inject_bitflip(ser, args.hex_file, args.bit_flip,
                                          args.count, args.delay)
            elif args.burst:
                start_bit, num_bits = args.burst
                injected = inject_burst(ser, args.hex_file, start_bit, num_bits,
                                        args.count, args.delay)
            else:
                injected = inject_hex_file(ser, args.hex_file, args.delay)
            print(f"\nTotal frames injected: {injected}")
            return 0

        # No action specified — show status
        resp = send_command(ser, "STATUS")
        print(resp.strip() or "(no response)")
        return 0

    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
