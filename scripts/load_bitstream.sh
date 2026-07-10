#!/usr/bin/env bash
# load_bitstream.sh — program a bitstream onto the connected AlSaqr FPGA
#
# Self-contained version for the alsaqr-software bundle. Mirrors the
# alsaqr-fpga `make load_bistream` flow, but as a standalone script (the
# bundle has no root Makefile — it uses scripts/ wrappers like build.sh/load.sh).
#
# Usage:
#   ./scripts/load_bitstream.sh <bitstream_file>
#   ./scripts/load_bitstream.sh bitstreams/alsaqr_xilinx_47.bit
#   ./scripts/load_bitstream.sh                       # lists available .bit files
#
# Behaviour:
#   - Sources Vivado settings if neither vivado_lab nor vivado is on PATH.
#   - Picks vivado_lab if available, else full vivado (both can program in batch).
#   - Auto-detects the connected Digilent/Olimex JTAG target (no hardcoded serial).
#     Override with HW_TARGET=xilinx_tcf/Digilent/<serial>.
#
# Env overrides:
#   VIVADO            Force the Vivado binary (e.g. VIVADO=vivado_lab)
#   VIVADO_SETTINGS   Path to settings64.sh to source if Vivado not on PATH
#                     (default: /tools/Xilinx/Vivado/2024.2/settings64.sh)
#   HW_TARGET         Skip auto-detect and use this JTAG target
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALSAQR_ROOT="$(dirname "$SCRIPT_DIR")"

# --- Make a Vivado binary available -----------------------------------------
# Allow VIVADO_LAB_SETTINGS as a legacy alias for VIVADO_SETTINGS.
VIVADO_SETTINGS="${VIVADO_SETTINGS:-${VIVADO_LAB_SETTINGS:-/tools/Xilinx/Vivado/2024.2/settings64.sh}}"
if ! command -v vivado_lab >/dev/null 2>&1 && ! command -v vivado >/dev/null 2>&1; then
    if [[ -f "$VIVADO_SETTINGS" ]]; then
        # shellcheck source=/dev/null
        source "$VIVADO_SETTINGS"
    else
        echo "Error: no vivado/vivado_lab on PATH and settings not found at:" >&2
        echo "         $VIVADO_SETTINGS" >&2
        echo "       Set VIVADO_SETTINGS to your Vivado settings64.sh." >&2
        exit 1
    fi
fi

# Pick the binary: explicit $VIVADO wins, else prefer vivado_lab, else vivado.
if [[ -n "${VIVADO:-}" ]]; then
    VIVADO_BIN="$VIVADO"
elif command -v vivado_lab >/dev/null 2>&1; then
    VIVADO_BIN="vivado_lab"
else
    VIVADO_BIN="vivado"
fi
if ! command -v "$VIVADO_BIN" >/dev/null 2>&1; then
    echo "Error: '$VIVADO_BIN' not found on PATH after sourcing settings." >&2
    exit 1
fi

# --- Resolve the bitstream argument -----------------------------------------
if [[ -z "${1:-}" ]]; then
    echo "Usage: $0 <bitstream_file>"
    echo "Example: $0 bitstreams/alsaqr_xilinx_47.bit"
    echo ""
    echo "Available bitstreams:"
    ls -1t "$ALSAQR_ROOT/bitstreams/"*.bit 2>/dev/null | sed 's/^/  /' | head -25
    exit 1
fi

BITSTREAM="$1"
if [[ ! "$BITSTREAM" = /* ]]; then
    BITSTREAM="$ALSAQR_ROOT/$BITSTREAM"
fi
if [[ ! -f "$BITSTREAM" ]]; then
    echo "Error: bitstream file not found: $BITSTREAM" >&2
    exit 1
fi

# --- Print stats parsed from the .bit header --------------------------------
print_bitstream_info() {
    local f="$1" size_bytes size_human mtime hdr design part bdate btime ver
    size_bytes=$(stat -c%s "$f")
    size_human=$(numfmt --to=iec --suffix=B "$size_bytes" 2>/dev/null || echo "${size_bytes}B")
    mtime=$(stat -c%y "$f" | cut -d. -f1)

    hdr=$(head -c 256 "$f" | strings -n 3 2>/dev/null || true)
    design=$(awk -F';' '/UserID=/{ sub(/^\//,"",$1); print $1; exit }' <<<"$hdr")
    ver=$(awk -F';' '/Version=/{ for(i=1;i<=NF;i++) if($i~/^Version=/){ sub(/^Version=/,"",$i); print $i; exit } }' <<<"$hdr")
    part=$(awk '/^xc[a-z0-9]+-/{ print; exit }' <<<"$hdr")
    bdate=$(awk '/^[0-9]{4}\/[0-9]{2}\/[0-9]{2}/{ print; exit }' <<<"$hdr")
    btime=$(awk '/^[ \t]*[0-9]{2}:[0-9]{2}:[0-9]{2}/{ gsub(/^[ \t]+/,"",$0); print; exit }' <<<"$hdr")

    echo "Bitstream stats:"
    echo "  File:        $f"
    echo "  Size:        $size_human ($size_bytes bytes)"
    echo "  Modified:    $mtime"
    [[ -n "$design" ]] && echo "  Design:      $design"
    [[ -n "$part"   ]] && echo "  Part:        $part"
    [[ -n "$ver"    ]] && echo "  Vivado:      $ver"
    if [[ -n "$bdate" || -n "$btime" ]]; then
        echo "  Built:       ${bdate}${bdate:+ }${btime}"
    fi
    echo ""
}
print_bitstream_info "$BITSTREAM"

# --- Auto-detect JTAG target unless caller forced one -----------------------
HW_TARGET="${HW_TARGET:-}"
if [[ -z "$HW_TARGET" ]]; then
    echo "Detecting connected JTAG target (via $VIVADO_BIN)…"
    DETECT_TCL=$(mktemp --suffix=.tcl)
    DETECT_LOG=$(mktemp)
    trap 'rm -f "$DETECT_TCL" "$DETECT_LOG"' EXIT
    cat > "$DETECT_TCL" <<'EOF'
open_hw_manager
connect_hw_server -allow_non_jtag
foreach t [get_hw_targets] { puts "TARGET: $t" }
EOF
    "$VIVADO_BIN" -mode batch -nojournal -nolog -source "$DETECT_TCL" >"$DETECT_LOG" 2>&1 || true
    DETECTED=$(awk '/^TARGET: /{print $2; exit}' "$DETECT_LOG")
    if [[ -z "$DETECTED" ]]; then
        echo "Error: no JTAG target found by hw_server. Is the board powered/connected?" >&2
        echo "Last lines of $VIVADO_BIN output:" >&2
        tail -20 "$DETECT_LOG" | sed 's/^/  /' >&2
        exit 1
    fi
    # Strip leading "localhost:3121/" if present; the .tcl matches with a wildcard
    HW_TARGET="${DETECTED#localhost:*/}"
    echo "Found target: $HW_TARGET"
fi

echo "Loading bitstream: $BITSTREAM"
echo "Target:            $HW_TARGET"
echo "Vivado:            $VIVADO_BIN"
echo ""

"$VIVADO_BIN" -mode batch -source "$ALSAQR_ROOT/scripts/load_bistream.tcl" -tclargs "$BITSTREAM" "$HW_TARGET"
