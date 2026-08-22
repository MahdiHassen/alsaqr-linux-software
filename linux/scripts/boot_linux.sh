#!/usr/bin/env bash
# Boot Linux on AlSaqr FPGA over JTAG using bundled OpenOCD + GDB.
#
# Reproduces alsaqr-fpga `make openocd` + `make gdb-load-payload` in a single
# script. Uploads DTB to 0x81800000, parks each HART at _start=0x80000000,
# loads fw_payload.elf, continues.
#
# Defaults to the pre-built artifacts in linux/prebuilt/. Override with env:
#   PAYLOAD=/path/to/fw_payload.elf  DTB=/path/to/alsaqr.dtb  ./boot_linux.sh
#
# UART on host: 38400 8N1. Use picocom/minicom in a separate terminal.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ALSAQR_ROOT="$(cd "$LINUX_DIR/.." && pwd)"

# Pull in bundled openocd + gdb + LD_LIBRARY_PATH
if [ -z "${ALSAQR_ROOT_SOURCED:-}" ]; then
    # shellcheck disable=SC1091
    source "$ALSAQR_ROOT/source.sh" >/dev/null
fi

OPENOCD_BIN="$ALSAQR_ROOT/openocd/bin/openocd"
OPENOCD_DIR="$ALSAQR_ROOT/openocd"
GDB_BIN_LOCAL="${GDB_BIN:-$ALSAQR_ROOT/toolchain/rv64/bin/riscv64-unknown-elf-gdb}"
export OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-$ALSAQR_ROOT/openocd/tcl}"

PAYLOAD="${PAYLOAD:-$LINUX_DIR/prebuilt/fw_payload.elf}"
DTB="${DTB:-$LINUX_DIR/prebuilt/alsaqr.dtb}"
DTB_ADDR="${DTB_ADDR:-0x81800000}"
GDB_PORT="${GDB_PORT:-6699}"
# OpenOCD TCL RPC port. Left open so scripts/send_file.sh can push data into the
# running Linux while GDB sits in `continue` (see RUNBOOK §8). Set to "disabled"
# to restore the old behaviour.
TCL_PORT="${TCL_PORT:-6666}"
ADAPTER_SPEED="${ADAPTER_SPEED:-2000}"
INTERFACE="${INTERFACE:-ftdi}"
DEVICE="${DEVICE:-olimex-arm-usb-ocd-h}"
ID="${ID:-}"
TARGET="${TARGET:-smp}"
NUM_HARTS="${NUM_HARTS:-4}"
export NUM_HARTS

[ -f "$PAYLOAD" ] || { echo "fw_payload.elf not found: $PAYLOAD" >&2; exit 1; }
[ -f "$DTB" ]     || { echo "alsaqr.dtb not found: $DTB" >&2; exit 1; }
[ -x "$OPENOCD_BIN" ] || { echo "openocd not found: $OPENOCD_BIN" >&2; exit 1; }
[ -x "$GDB_BIN_LOCAL" ] || { echo "gdb not found: $GDB_BIN_LOCAL" >&2; exit 1; }

TMPDIR="$(mktemp -d -t alsaqr-linux-boot-XXXXXX)"
OCD_LOG="$TMPDIR/openocd.log"

OPENOCD_ARGS=(
    -s "$OPENOCD_DIR"
    -c "adapter speed $ADAPTER_SPEED"
    -c "set _INTERFACE $INTERFACE"
    -c "set _DEVICE $DEVICE"
    -c "gdb_port $GDB_PORT"
    -c "tcl_port $TCL_PORT"
    -c "telnet_port disabled"
)
[ -n "$ID" ] && OPENOCD_ARGS+=( -c "adapter serial $ID" )
OPENOCD_ARGS+=(
    -c "set _NUM_CORES $NUM_HARTS"
    -c "set _TARGET $TARGET"
    -f "openocd.cfg"
    -c "init"
    -c "echo \"Ready for Remote Connections\""
)

echo "[*] Starting OpenOCD on :$GDB_PORT…"
"$OPENOCD_BIN" "${OPENOCD_ARGS[@]}" >"$OCD_LOG" 2>&1 &
OCD_PID=$!
cleanup() { kill "$OCD_PID" >/dev/null 2>&1 || true; wait "$OCD_PID" 2>/dev/null || true; rm -rf "$TMPDIR"; }
trap cleanup EXIT INT TERM

for _ in $(seq 1 120); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$GDB_PORT") 2>/dev/null; then exec 3>&-; break; fi
    if grep -q "Ready for Remote Connections" "$OCD_LOG" 2>/dev/null; then break; fi
    sleep 0.5
done
if ! (exec 3<>/dev/tcp/127.0.0.1/"$GDB_PORT") 2>/dev/null; then
    echo "[!] OpenOCD failed to start. Log:" >&2; cat "$OCD_LOG" >&2; exit 1
fi

# Build GDB cmd line: load DTB, park each hart at _start, load payload, continue
GDB_ARGS=( -q "$PAYLOAD"
    -ex "set pagination off"
    -ex "set confirm off"
    -ex "set remotetimeout 120"
    -ex "target extended-remote :$GDB_PORT"
    -ex "monitor load_image $DTB $DTB_ADDR"
)
for h in $(seq 1 "$NUM_HARTS"); do
    GDB_ARGS+=( -ex "thread $h" -ex "set \$pc=_start" )
done
GDB_ARGS+=( -ex "load" -ex "continue" )

echo "[*] Loading DTB @ $DTB_ADDR, parking $NUM_HARTS hart(s) at _start, loading payload…"
exec "$GDB_BIN_LOCAL" "${GDB_ARGS[@]}"
