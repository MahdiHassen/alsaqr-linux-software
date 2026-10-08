#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# send_file.sh — push a file from the host into running CVA6 Linux over JTAG.
#
# Works WHILE Linux is running and WHILE boot_linux.sh's GDB sits in `continue`.
# That is possible because openocd/target/smp.cfg sets `riscv set_mem_access
# sysbus`: the debug module's system-bus master drives the SoC AXI crossbar
# directly, independent of the harts, so no halt is needed.
#
# The file lands in the 16 MB window at 0x9f000000, which must be outside
# Linux's memory: boot with DTB= set to a tree built from
# linux/cva6-sdk/configs/alsaqr.dts (boot_linux.sh then hands it to Linux; the
# default prebuilt boot runs on the boot ROM's tree, where the window is Linux
# RAM). Receive it on the target with linux/recv/recv.
#
# Wire format at 0x9f000000:
#     +0x0000  magic   'ALSQ' (0x51534c41)  <- written LAST
#     +0x0004  length  payload bytes
#     +0x1000  payload
#
# Magic-last is the entire protocol: the receiver only acts once magic appears,
# so it can never observe a half-written payload.
#
# Usage:
#   ./scripts/send_file.sh <file>          # push
#   ./scripts/send_file.sh --status        # read back the header
#   ./scripts/send_file.sh --clear         # zero magic (mark window consumed)
#   ./scripts/send_file.sh --cmd 'targets' # run any OpenOCD command (debugging)
#
# Env:
#   TCL_PORT   OpenOCD TCL RPC port (default 6666, matches boot_linux.sh)
#   TCL_HOST   default 127.0.0.1
#   WIN_BASE   landing zone base (default 0x9f000000)
# -----------------------------------------------------------------------------
set -euo pipefail

TCL_HOST="${TCL_HOST:-127.0.0.1}"
TCL_PORT="${TCL_PORT:-6666}"
WIN_BASE="${WIN_BASE:-0x9f000000}"

MAGIC=0x51534c41                                    # 'ALSQ'
WIN_SIZE=$((16 * 1024 * 1024))
HDR_SIZE=0x1000
ADDR_MAGIC=$WIN_BASE
ADDR_LEN=$(printf '0x%x' $((WIN_BASE + 4)))
ADDR_PAYLOAD=$(printf '0x%x' $((WIN_BASE + HDR_SIZE)))
MAX_PAYLOAD=$((WIN_SIZE - HDR_SIZE))

# OpenOCD's TCL RPC framing: each command and each reply is terminated by 0x1a.
# One command per connection — simplest thing that is reliable, and the
# per-connection cost is irrelevant next to JTAG transfer time.
#
# Uses bash's /dev/tcp rather than nc on purpose: nc variants disagree about
# whether stdin EOF closes the connection (openbsd nc needs -N, netcat-traditional
# needs -q0), so a plain `printf | nc` would stall until the timeout on every
# single command. /dev/tcp has no such ambiguity and no external dependency.
ocd() {
    local cmd="$1" reply=''
    exec 3<>"/dev/tcp/$TCL_HOST/$TCL_PORT" || {
        echo "error: cannot reach OpenOCD TCL server at $TCL_HOST:$TCL_PORT" >&2
        return 1
    }
    printf '%s\x1a' "$cmd" >&3
    # No timeout: load_image of a large file legitimately takes minutes over JTAG.
    IFS= read -r -d $'\032' -u 3 reply || true
    exec 3>&-
    printf '%s' "$reply"
}

require_server() {
    if ! (exec 3<>"/dev/tcp/$TCL_HOST/$TCL_PORT") 2>/dev/null; then
        cat >&2 <<EOF
error: no OpenOCD TCL server on $TCL_HOST:$TCL_PORT

  Boot Linux first (it leaves the port open):
      ./linux/scripts/boot_linux.sh

  If you started OpenOCD by hand, add:  -c "tcl_port $TCL_PORT"
EOF
        exit 1
    fi
    exec 3>&-
}

# Read one 32-bit word. `mdw <addr>` replies like: 0x9f000000: 51534c41
read_word() {
    ocd "mdw $1" | sed -n 's/.*: *\([0-9a-fA-F][0-9a-fA-F]*\).*/0x\1/p' | head -1
}

show_status() {
    local m l
    m="$(read_word "$ADDR_MAGIC")"
    l="$(read_word "$ADDR_LEN")"
    echo "window   : $WIN_BASE (+${HDR_SIZE} payload)"
    echo "magic    : ${m:-<no reply>}"
    echo "length   : ${l:-<no reply>}$([ -n "${l:-}" ] && printf ' (%d bytes)' "$((l))")"
    if [[ "${m:-}" == "0x51534c41" ]]; then
        echo "state    : READY — a file is waiting to be received"
    elif [[ "${m:-}" == "0x0" || "${m:-}" == "0x00000000" ]]; then
        echo "state    : empty"
    else
        cat <<EOF
state    : UNEXPECTED magic

  If this is garbage rather than 0x0, the window may not be backed by DRAM.
  Note the SoC ties off system-bus error reporting (cva6_subsystem.sv:357-358),
  so a bad address returns junk silently instead of raising sberror. Check that
  WIN_BASE is inside DDR and outside Linux's memory node.
EOF
    fi
}

case "${1:-}" in
    -h|--help) sed -n '2,36p' "$0"; exit 0;;
    --status)  require_server; show_status; exit 0;;
    --cmd)     require_server; shift; [[ $# -gt 0 ]] || { echo "error: --cmd needs an OpenOCD command" >&2; exit 1; }
               ocd "$*"; echo; exit 0;;
    --clear)   require_server; ocd "mww $ADDR_MAGIC 0" >/dev/null; echo "[ok] magic cleared"; exit 0;;
    "")        echo "usage: $0 <file> | --status | --clear   (see --help)" >&2; exit 1;;
esac

IMG="$1"
[[ -f "$IMG" ]] || { echo "error: no such file: $IMG" >&2; exit 1; }
IMG_ABS="$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")"

SIZE=$(wc -c <"$IMG_ABS" | tr -d ' ')
if (( SIZE == 0 )); then
    echo "error: $IMG is empty" >&2; exit 1
fi
if (( SIZE > MAX_PAYLOAD )); then
    echo "error: $IMG is $SIZE bytes, window holds at most $MAX_PAYLOAD" >&2; exit 1
fi

require_server

# Clear magic first so a receiver polling right now cannot latch the old length
# against the new payload.
ocd "mww $ADDR_MAGIC 0" >/dev/null

echo "[*] Sending $IMG ($SIZE bytes) to $ADDR_PAYLOAD …"
START=$(date +%s)

# load_image issues bulk auto-incrementing system-bus bursts. Do NOT replace this
# with a loop of mww — word-at-a-time over JTAG is orders of magnitude slower.
RESP="$(ocd "load_image {$IMG_ABS} $ADDR_PAYLOAD bin")"
if printf '%s' "$RESP" | grep -qiE 'error|invalid|can.?t|failed'; then
    echo "error: load_image failed:" >&2
    printf '%s\n' "$RESP" >&2
    exit 1
fi

ocd "mww $ADDR_LEN $SIZE" >/dev/null
ocd "mww $ADDR_MAGIC $MAGIC" >/dev/null      # magic LAST — publishes the payload

ELAPSED=$(( $(date +%s) - START ))
(( ELAPSED > 0 )) && RATE=" ($((SIZE / ELAPSED / 1024)) KiB/s)" || RATE=""

echo "[ok] sent $SIZE bytes in ${ELAPSED}s${RATE}"
echo
echo "On the Linux console:"
echo "    devmem $ADDR_MAGIC 32        # expect 0x51534C41"
echo "    recv -w -o /tmp/$(basename "$IMG")"
