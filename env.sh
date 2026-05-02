# Source this file to use the local Amiga cross-toolchain (vbcc + vasm + vlink).
#   . ./env.sh
# After sourcing, build with `make`. See BUILDING.md for the toolchain install.

# Resolve the directory this script lives in, regardless of CWD when sourced.
# BASH_SOURCE works in bash; falls back to $0 in plain sh.
_GRADIENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

export VBCC="$_GRADIENT_DIR/toolchain"
export PATH="$VBCC/bin:$PATH"

# vbcc's aos68k config refers to AmigaDOS-style "assigns" (vincludeos3:, vlibos3:).
# vbcc resolves these from environment variables of the same name.
export VINCLUDEOS3="$VBCC/targets/m68k-amigaos/include"
export VLIBOS3="$VBCC/targets/m68k-amigaos/lib"

unset _GRADIENT_DIR
