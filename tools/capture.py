"""Bounded serial capture for KAST bench verification.

Opens the CH340 COM port, records everything for a fixed window, prints
it, exits. Companion to `idf.py flash` — flash resets the board, this
catches the boot log without an interactive monitor session.

Usage: python capture.py COM3 25
"""
import sys
import time

import serial


def main():
    port_name = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

    port = serial.Serial(port_name, 115200, timeout=0.2)
    port.reset_input_buffer()
    print(f"[capture] {port_name} open, recording {seconds:.0f}s", flush=True)

    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        chunk = port.read(4096)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            buf += chunk
    port.close()
    print(f"\n[capture] done, {len(buf)} bytes", flush=True)


if __name__ == "__main__":
    main()
