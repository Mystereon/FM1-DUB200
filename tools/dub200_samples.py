#!/usr/bin/env python3
"""DUB-200 PCM slot manager for the M-VAVE FM-1.

DUB-200 has ten independent global PCM slots: A and B for Chord, Pings,
Bass, Drums and Noise. Project/piece settings select Off, A, B or alternating
A/B; the audio itself is kept in a protected raw-flash bank.

This tool deliberately requires an explicit --slot. Uploading stops the
transport and erases *only* that target slot before it transfers the new
asset. It never touches projects, panel calibration or the firmware image.

Requirements:
  ffmpeg (for conversion from any common audio format)
  python3 -m pip install mido python-rtmidi

Examples:
  python3 tools/dub200_samples.py --list
  python3 tools/dub200_samples.py --slot 6 --name amen --input amen.wav
  python3 tools/dub200_samples.py --slot 7 --name stab --input stab.flac \
      --midi-out 'FM-1 MIDI 1' --midi-in 'FM-1 MIDI 1'
  python3 tools/dub200_samples.py --clear 7
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
import zlib
from pathlib import Path

SLOTS = (
    "Chord A", "Chord B", "Pings A", "Pings B", "Bass A",
    "Bass B", "Drums A", "Drums B", "Noise A", "Noise B",
)
RATE = 11_025
MAX_BYTES = 0x6000 - 256
MFG = (0x7D, 0x44)
CMD_BEGIN, CMD_DATA, CMD_COMMIT, CMD_CLEAR = 1, 2, 3, 4


def pack7(raw: bytes) -> bytes:
    """Pack 8-bit payload bytes into legal seven-bit MIDI SysEx bytes."""
    acc = 0
    bits = 0
    out = bytearray()
    for byte in raw:
        acc |= byte << bits
        bits += 8
        while bits >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            bits -= 7
    if bits:
        out.append(acc & 0x7F)
    return bytes(out)


def pcm8(path: Path, trim: bool) -> bytes:
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is required to convert audio. Install it, then try again.")
    run = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:a:0", "-ac", "1", "-ar", str(RATE), "-f", "u8", "-"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if run.returncode:
        raise SystemExit(f"ffmpeg could not convert {path}:\n{run.stderr.decode(errors='replace')}")
    data = run.stdout
    if not data:
        raise SystemExit("The input had no decodable audio.")
    if len(data) > MAX_BYTES:
        seconds = MAX_BYTES / RATE
        if not trim:
            raise SystemExit(
                f"Converted audio is {len(data)} bytes ({len(data)/RATE:.2f}s), but a slot holds {MAX_BYTES} bytes "
                f"({seconds:.2f}s). Trim your source or pass --trim to keep its first {seconds:.2f}s."
            )
        data = data[:MAX_BYTES]
    return data


def wait_reply(port, command: int, slot: int, timeout: float) -> None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for message in port.iter_pending():
            if message.type != "sysex":
                continue
            data = tuple(message.data)
            if len(data) == 6 and data[:4] == MFG + (0x70, command) and data[5] == slot:
                if data[4]:
                    raise RuntimeError(f"FM-1 rejected command {command} for slot {slot} (status {data[4]}).")
                return
        time.sleep(0.002)
    raise RuntimeError(
        "No DUB-200 acknowledgement. Confirm the DUB-200 firmware is installed, the MIDI input/output port names are correct, "
        "and no other MIDI program has the FM-1 port open."
    )


def send(outport, inport, command: int, payload: bytes, slot: int, timeout: float) -> None:
    import mido
    outport.send(mido.Message("sysex", data=MFG + (command,) + tuple(pack7(payload))))
    wait_reply(inport, command, slot, timeout)


def ports() -> tuple[str, str]:
    try:
        import mido
    except ModuleNotFoundError as exc:
        raise SystemExit("Install MIDI support first: python3 -m pip install mido python-rtmidi") from exc
    print("MIDI output ports:")
    for name in mido.get_output_names():
        print(f"  {name}")
    print("MIDI input ports:")
    for name in mido.get_input_names():
        print(f"  {name}")
    return "", ""


def choose_port(names: list[str], supplied: str | None, kind: str) -> str:
    if supplied:
        return supplied
    fm = [name for name in names if "fm-1" in name.lower() or "m-vave" in name.lower()]
    if len(fm) == 1:
        return fm[0]
    if len(names) == 1:
        return names[0]
    raise SystemExit(f"Specify --midi-{kind}. Use --list to show available ports.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload or clear one DUB-200 user PCM slot over USB MIDI.")
    group = parser.add_mutually_exclusive_group(required=False)
    group.add_argument("--slot", type=int, help="upload target, 0–9 (see --list)")
    group.add_argument("--clear", type=int, metavar="SLOT", help="erase one slot, 0–9")
    parser.add_argument("--input", type=Path, help="audio file to convert and upload")
    parser.add_argument("--name", help="up to 16 ASCII characters stored as the slot label")
    parser.add_argument("--trim", action="store_true", help="allow truncation to the 2.21-second per-slot maximum")
    parser.add_argument("--midi-out", help="MIDI output port (auto-selects a single FM-1-like port if possible)")
    parser.add_argument("--midi-in", help="MIDI input port used for DUB-200 acknowledgements")
    parser.add_argument("--list", action="store_true", help="list slots and detected MIDI ports, then exit")
    args = parser.parse_args()

    print(f"DUB-200 slot format: mono unsigned PCM8, {RATE:,} Hz, {MAX_BYTES:,} bytes / {MAX_BYTES/RATE:.2f} seconds")
    print("Slots:")
    for index, label in enumerate(SLOTS):
        print(f"  {index}: {label}")
    if args.list:
        ports()
        return 0
    if args.slot is None and args.clear is None:
        parser.error("choose --slot N to upload or --clear N")
    slot = args.clear if args.clear is not None else args.slot
    if slot is None or not 0 <= slot < len(SLOTS):
        parser.error("slot must be an integer from 0 through 9")
    if args.clear is None and not args.input:
        parser.error("--input is required when uploading")
    if args.clear is None and not args.input.is_file():
        parser.error(f"input file not found: {args.input}")

    try:
        import mido
    except ModuleNotFoundError as exc:
        raise SystemExit("Install MIDI support first: python3 -m pip install mido python-rtmidi") from exc
    outname = choose_port(mido.get_output_names(), args.midi_out, "out")
    inname = choose_port(mido.get_input_names(), args.midi_in, "in")
    with mido.open_output(outname) as outport, mido.open_input(inname) as inport:
        if args.clear is not None:
            print(f"Clearing slot {slot} ({SLOTS[slot]}); playback will stop briefly.")
            send(outport, inport, CMD_CLEAR, bytes((slot,)), slot, timeout=8.0)
            print("Done.")
            return 0

        data = pcm8(args.input, args.trim)
        try:
            label = (args.name or args.input.stem).encode("ascii", "strict")[:16]
        except UnicodeEncodeError:
            raise SystemExit("--name must use ASCII characters only (maximum 16).")
        label = label.ljust(16, b"\0")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        print(f"Importing {len(data):,} bytes ({len(data)/RATE:.2f}s) to slot {slot} ({SLOTS[slot]}) as {label.rstrip(bytes((0,))).decode()!r}.")
        print("Playback stops while the target slot is erased and the new asset is committed.")
        begin = bytes((slot,)) + len(data).to_bytes(4, "little") + crc.to_bytes(4, "little") + label
        send(outport, inport, CMD_BEGIN, begin, slot, timeout=8.0)
        for offset in range(0, len(data), 40):
            chunk = data[offset : offset + 40]
            send(outport, inport, CMD_DATA, bytes((slot,)) + offset.to_bytes(4, "little") + chunk, slot, timeout=2.0)
            if offset == 0 or offset + len(chunk) == len(data) or offset % 2000 == 0:
                print(f"  {offset + len(chunk):5d}/{len(data):5d} bytes", end="\r", flush=True)
        print()
        send(outport, inport, CMD_COMMIT, bytes((slot,)), slot, timeout=8.0)
        print("Committed and CRC-verified by DUB-200. Select the layer, hold GLO and turn PRESETS to choose A, B or ALT; save the piece to retain that choice.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nCancelled. An interrupted slot remains invalid/silent until its next complete upload.", file=sys.stderr)
        raise SystemExit(130)
    except RuntimeError as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)
