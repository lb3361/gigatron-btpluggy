#!/usr/bin/env python3
"""btpluggy.py -- small esptool wrapper for the BtPluggy flash partitions.

Requires the `esptool` Python module:  pip install esptool

Usage:
  btpluggy.py help                 # list commands
  btpluggy.py readfat  IMAGE       # dump the read-only FAT partition to IMAGE
  btpluggy.py writefat IMAGE       # flash IMAGE into the FAT partition

Partition offsets/sizes are always read from the device's live partition table
(at 0x8000, override with --ptab-offset). If it cannot be read or parsed the
command aborts -- offsets are never guessed, so a write can never go to the
wrong place. The serial port is auto-detected unless -p/--port is given.

To add a command (e.g. reading the SPIFFS partition, or clearing NVS bonds),
write a cmd_<name>() with a docstring (its first line becomes the summary in
`help`), call esptool_read()/esptool_write()/..., and register a subparser for
it in main().
"""

import os
import sys
import struct
import tempfile
import textwrap
import argparse

# --- esptool availability -------------------------------------------------
try:
    import esptool
except ImportError:
    sys.exit(
        "error: the 'esptool' Python module is not installed.\n"
        "       Install it with:  pip install esptool"
    )
try:
    from esptool.util import FatalError
except Exception:                       # pragma: no cover - defensive
    FatalError = RuntimeError

DEFAULT_CHIP = "esp32"
DEFAULT_BAUD = 230400

# On-flash partition table (see ESP-IDF gen_esp32part.py):
#   32-byte entries, little-endian: magic(2) type(1) subtype(1) offset(4)
#   size(4) label(16) flags(4); end marker is 32 * 0xFF.
PTAB_OFFSET_DEFAULT = 0x8000            # CONFIG_PARTITION_TABLE_OFFSET
PTAB_READ_SIZE = 0x1000                 # one flash sector
PTAB_MAGIC = b"\xaa\x50"
PTAB_END = b"\xff" * 32
PTAB_STRUCT = struct.Struct("<2sBBLL16sL")


# --- partition table parsing ---------------------------------------------
def parse_partition_table(data):
    """Parse a raw partition-table blob into {name: (offset, size)}."""
    parts = {}
    for o in range(0, len(data), 32):
        entry = data[o:o + 32]
        if len(entry) != 32 or entry == PTAB_END:
            break
        if entry[:2] != PTAB_MAGIC:
            continue                    # MD5 entry or padding
        _magic, _type, _subtype, offset, size, name, _flags = PTAB_STRUCT.unpack(entry)
        if b"\x00" in name:
            name = name[:name.index(b"\x00")]
        parts[name.decode("ascii", "replace")] = (offset, size)
    return parts


def _partition(parts, name):
    if name not in parts:
        sys.exit(f"error: partition '{name}' not found in the device partition table")
    return parts[name]


# --- esptool invocation ---------------------------------------------------
def _run_esptool(argv, quiet=False):
    """Invoke esptool's CLI entry point in-process; return its exit code."""
    try:
        esptool.main(argv)
    except SystemExit as e:                # esptool re-raises non-zero exits
        return e.code if isinstance(e.code, int) else 1
    except FatalError as e:
        if not quiet:
            print(f"error: {e}", file=sys.stderr)
        return 2
    return 0


def _global_args(port, chip, baud=DEFAULT_BAUD):
    argv = [ "--chip", chip ]
    if port:
        argv += [ "--port", port ]
    if baud:
        argv += [ "--baud", str(baud) ]
    return argv


def esptool_read(offset, size, outfile, port=None, chip=DEFAULT_CHIP):
    """Read `size` bytes at flash `offset` into `outfile`."""
    return _run_esptool(_global_args(port, chip)
                        + ["read-flash", hex(offset), hex(size), outfile])


def esptool_write(offset, infile, port=None, chip=DEFAULT_CHIP):
    """Flash `infile` at flash `offset`."""
    return _run_esptool(_global_args(port, chip)
                        + ["write-flash", hex(offset), infile])


def load_partitions_from_device(port, chip, ptab_offset=PTAB_OFFSET_DEFAULT):
    """Read and parse the live partition table from the device."""
    fd, tmp = tempfile.mkstemp(suffix=".ptab.bin")
    os.close(fd)
    try:
        rc = _run_esptool(_global_args(port, chip)
                          + ["--silent"]
                          + ["read-flash", hex(ptab_offset), hex(PTAB_READ_SIZE), tmp],
                          quiet=True)
        if rc != 0:
            raise RuntimeError(f"esptool could not read the partition table (exit {rc})")
        with open(tmp, "rb") as f:
            data = f.read()
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    parts = parse_partition_table(data)
    if not parts:
        raise RuntimeError(
            f"no valid partition entries found at {hex(ptab_offset)} "
            f"(wrong --ptab-offset, or the table is not programmed)")
    return parts


# --- commands -------------------------------------------------------------
def cmd_readfat(args, parts):
    """Dump the read-only FAT partition to an image file.

    Reads the whole 'fat' partition (offset and size taken from the device's
    live partition table) into IMAGE. Useful for backing up the FAT image that
    holds the loader programs, or inspecting it on a PC.
    """
    offset, size = _partition(parts, "fat")
    print(f"fat: offset={hex(offset)} size={hex(size)}", file=sys.stderr)
    return esptool_read(offset, size, args.image, port=args.port, chip=args.chip)


def cmd_writefat(args, parts):
    """Flash an image file into the read-only FAT partition.

    Writes IMAGE to the 'fat' partition (offset and size from the device's
    live partition table). The image must not be larger than the partition.
    This is how you install the FAT image that holds the loader programs.
    """
    offset, size = _partition(parts, "fat")
    try:
        actual = os.path.getsize(args.image)
    except OSError as e:
        sys.exit(f"error: cannot read {args.image}: {e}")
    if actual > size:
        sys.exit(f"error: {args.image} is {actual} bytes, "
                 f"larger than the 'fat' partition ({size} bytes)")
    print(f"fat: offset={hex(offset)} size={hex(size)}", file=sys.stderr)
    return esptool_write(offset, args.image, port=args.port, chip=args.chip)


def cmd_help(args, parts):
    """List the available commands and what they do.

    With no argument, prints a one-line summary of every command (taken from
    each command function's docstring). With a command name, prints that
    command's full description.
    """
    sub = args.subparsers
    if args.topic:
        parser = sub.choices.get(args.topic)
        if parser is None:
            sys.exit(f"error: unknown command '{args.topic}'")
        func = parser.get_default("func")
        doc = (func.__doc__ or "(no description)").strip()
        print(f"{args.topic}:")
        print(textwrap.indent(doc, "  "))
        return 0
    print("btpluggy.py -- esptool wrapper for BtPluggy flash partitions\n")
    print("Commands:")
    for name, parser in sub.choices.items():
        func = parser.get_default("func")
        doc = (func.__doc__ or "").strip().splitlines()
        summary = doc[0] if doc else "(no description)"
        print(f"  {name:<10} {summary}")
    print("\nUse 'btpluggy.py help <command>' for details, "
          "or 'btpluggy.py <command> -h' for its options.")
    return 0


# --- CLI ------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="btpluggy.py",
        description="esptool wrapper for BtPluggy flash partitions.",
    )
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=None,
                        help="serial port (default: auto-detect)")
    common.add_argument("-c", "--chip", default=DEFAULT_CHIP,
                        help=f"target chip (default: {DEFAULT_CHIP})")
    common.add_argument("--ptab-offset", type=lambda s: int(s, 0),
                        default=PTAB_OFFSET_DEFAULT,
                        help="partition table offset on flash (default: 0x8000)")

    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("readfat", parents=[common],
                       help="read the FAT partition to an image file")
    p.add_argument("image", help="output image file")
    p.set_defaults(func=cmd_readfat, needs_parts=True)

    p = sub.add_parser("writefat", parents=[common],
                       help="write an image file to the FAT partition")
    p.add_argument("image", help="input image file")
    p.set_defaults(func=cmd_writefat, needs_parts=True)

    p = sub.add_parser("help", parents=[common],
                       help="list commands, or describe one")
    p.add_argument("topic", nargs="?", metavar="COMMAND",
                   help="command to describe")
    p.set_defaults(func=cmd_help, needs_parts=False, subparsers=sub)

    args = parser.parse_args(argv)
    parts = None
    if args.needs_parts:
        try:
            parts = load_partitions_from_device(args.port, args.chip, args.ptab_offset)
        except Exception as e:
            sys.exit(f"error: {e}")
    return args.func(args, parts)


if __name__ == "__main__":
    sys.exit(main())
