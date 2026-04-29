#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# load.sh — load and run a .riscv binary on AlSaqr FPGA via OpenOCD + GDB
#
# Self-contained version for the alsaqr-software bundle. All paths resolved
# relative to this script — no external Makefile or toolchain needed beyond
# what is shipped in this bundle.
#
# Flow:
#   1. Start OpenOCD in the background (GDB server on :GDB_PORT)
#   2. Launch GDB with a script that:
#      - target extended-remote :GDB_PORT
#      - monitor reset halt
#      - load the ELF (or 'restore' for raw binary)
#      - resume / sleep / halt
#      - dump_image exp.dump 0x85000000 0x40000
#      - x/128wx 0x85000000
#   3. Drop into interactive GDB
#
# Usage:
#   ./load.sh /path/to/firmware.riscv                    # ELF
#   ./load.sh /path/to/boot.bin --raw --addr 0x80000000  # raw binary
#
# Options:
#   --gdb-port PORT     OpenOCD GDB server port (default 6699)
#   --addr 0xADDR       Base address for raw binaries (default 0x80000000)
#   --raw               Force treating the input as raw binary
#   --time MS           ms to run before halt/dump (default 5000)
#   --counters          Dump PMU counter MMIO regions (counters 0–11)
# -----------------------------------------------------------------------------
set -euo pipefail

# Resolve bundle root regardless of where we're invoked from
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALSAQR_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OPENOCD_BIN="$ALSAQR_ROOT/openocd/bin/openocd"
OPENOCD_DIR="$ALSAQR_ROOT/openocd"
GDB_BIN_LOCAL="${GDB_BIN:-$ALSAQR_ROOT/toolchain/rv64/bin/riscv64-unknown-elf-gdb}"
export OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-$ALSAQR_ROOT/openocd/tcl}"

# Defaults (matching alsaqr-fpga/Makefile)
GDB_PORT="${GDB_PORT:-6699}"
ADAPTER_SPEED="${ADAPTER_SPEED:-2000}"
INTERFACE="${INTERFACE:-ftdi}"
DEVICE="${DEVICE:-olimex-arm-usb-ocd-h}"
ID="${ID:-}"
TARGET="${TARGET:-smp}"
NUM_HARTS="${NUM_HARTS:-4}"
export NUM_HARTS

MEM_BASE_ADDR="${MEM_BASE_ADDR:-0x80000000}"
RUN_TIME_MS="${RUN_TIME_MS:-5000}"
FORCE_RAW=0
SHOW_COUNTERS=0

IMG="${1:-}"
shift || true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --addr)     MEM_BASE_ADDR="$2"; shift 2;;
        --gdb-port) GDB_PORT="$2"; shift 2;;
        --time)     RUN_TIME_MS="$2"; shift 2;;
        --raw)      FORCE_RAW=1; shift;;
        --counters) SHOW_COUNTERS=1; shift;;
        -h|--help)  sed -n '1,40p' "$0"; exit 0;;
        *) echo "Unknown arg: $1" >&2; exit 1;;
    esac
done

if [[ -z "$IMG" || ! -f "$IMG" ]]; then
    echo "Error: supply a valid image path (.riscv ELF or raw .bin). See --help." >&2
    exit 1
fi
if ! [[ "$RUN_TIME_MS" =~ ^[0-9]+$ ]]; then
    echo "Error: --time must be an integer (ms)." >&2
    exit 1
fi
if [[ ! -x "$OPENOCD_BIN" ]]; then
    echo "Error: openocd not found at $OPENOCD_BIN" >&2
    exit 1
fi
if [[ ! -x "$GDB_BIN_LOCAL" ]]; then
    echo "Error: gdb not found at $GDB_BIN_LOCAL" >&2
    exit 1
fi

IMG_ABS="$(readlink -f "$IMG")"
TMPDIR="$(mktemp -d -t alsaqr-load-XXXXXX)"
GDB_CMDS="$TMPDIR/load_cmds.gdb"
OCD_LOG="$TMPDIR/openocd.log"

# Build OpenOCD argument list (matching alsaqr-fpga/Makefile)
OPENOCD_ARGS=(
    -s "$OPENOCD_DIR"
    -c "adapter speed $ADAPTER_SPEED"
    -c "set _INTERFACE $INTERFACE"
    -c "set _DEVICE $DEVICE"
    -c "gdb_port $GDB_PORT"
    -c "tcl_port disabled"
    -c "telnet_port disabled"
)
[[ -n "$ID" ]] && OPENOCD_ARGS+=( -c "adapter serial $ID" )
OPENOCD_ARGS+=(
    -c "set _NUM_CORES $NUM_HARTS"
    -c "set _TARGET $TARGET"
    -f "openocd.cfg"
    -c "init"
    -c "reset halt"
    -c "echo \"Ready for Remote Connections\""
)

echo "[*] Starting OpenOCD on :$GDB_PORT…"
"$OPENOCD_BIN" "${OPENOCD_ARGS[@]}" >"$OCD_LOG" 2>&1 &
OCD_PID=$!

cleanup() {
    echo
    echo "[*] Shutting down OpenOCD (pid $OCD_PID)…"
    kill "$OCD_PID" >/dev/null 2>&1 || true
    wait "$OCD_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

echo "[*] Waiting for OpenOCD GDB server on :$GDB_PORT …"
for _ in $(seq 1 120); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$GDB_PORT") 2>/dev/null; then exec 3>&-; break; fi
    if grep -q "Ready for Remote Connections" "$OCD_LOG" 2>/dev/null; then break; fi
    sleep 0.5
done
if ! (exec 3<>/dev/tcp/127.0.0.1/"$GDB_PORT") 2>/dev/null; then
    echo "[!] OpenOCD did not come up. Log:" >&2
    cat "$OCD_LOG" >&2
    exit 1
fi

# ELF or raw?
is_elf=0
if [[ $FORCE_RAW -eq 0 ]]; then
    if command -v readelf >/dev/null 2>&1 && readelf -h "$IMG_ABS" >/dev/null 2>&1; then
        is_elf=1
    elif command -v file >/dev/null 2>&1 && file -b "$IMG_ABS" | grep -q 'ELF'; then
        is_elf=1
    fi
fi
if [[ $is_elf -eq 1 ]]; then
    LOAD_SNIPPET="file \"$IMG_ABS\"
load"
else
    LOAD_SNIPPET="restore \"$IMG_ABS\" binary $MEM_BASE_ADDR"
fi

cat >"$GDB_CMDS" <<EOF
set pagination off
set confirm off
set remotetimeout 60
set arch riscv
target extended-remote :$GDB_PORT
monitor reset halt
$LOAD_SNIPPET
echo \n[ok] Connected, reset+halt, loaded: $IMG_ABS\n
monitor resume
monitor sleep $RUN_TIME_MS
monitor halt
monitor dump_image exp.dump 0x85000000 0x40000
echo [ok] Wrote exp.dump from 0x85000000\n
x/128wx 0x85000000
echo \n[ok] Ran ${RUN_TIME_MS}ms, halted, dumped, displayed 128 words at 0x85000000\n
echo \n=== DSPM (0x10428000) ===\n
x/32wx 0x10428000
echo \n=== ISPM (0x10427000) ===\n
x/32wx 0x10427000
echo \n=== PMU Control (0x10406000) ===\n
x/4wx 0x10406000
EOF

if [[ $SHOW_COUNTERS -eq 1 ]]; then
    cat >>"$GDB_CMDS" <<'COUNTERS'
echo \n=== PMU Timer (0x10405000) ===\n
x/4wx 0x10405000
echo \n=== Counter 0 (0x10407000) ===\n
x/4wx 0x10407000
echo \n=== Counter 1 (0x10408000) ===\n
x/4wx 0x10408000
echo \n=== Counter 2 (0x10409000) ===\n
x/4wx 0x10409000
echo \n=== Counter 3 (0x1040A000) ===\n
x/4wx 0x1040A000
echo \n=== Counter 4 (0x1040B000) ===\n
x/4wx 0x1040B000
echo \n=== Counter 5 (0x1040C000) ===\n
x/4wx 0x1040C000
echo \n=== Counter 6 (0x1040D000) ===\n
x/4wx 0x1040D000
echo \n=== Counter 7 (0x1040E000) ===\n
x/4wx 0x1040E000
echo \n=== Counter 8 (0x1040F000) ===\n
x/4wx 0x1040F000
echo \n=== Counter 9 (0x10410000) ===\n
x/4wx 0x10410000
echo \n=== Counter 10 (0x10411000) ===\n
x/4wx 0x10411000
echo \n=== Counter 11 (0x10412000) ===\n
x/4wx 0x10412000
COUNTERS
fi

echo "[*] Launching GDB (run time: ${RUN_TIME_MS} ms)…"
exec "$GDB_BIN_LOCAL" -q -x "$GDB_CMDS"
