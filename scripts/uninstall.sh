#!/bin/bash
# uninstall.sh — remove termchan binary, repo, and optionally config

set -e

DIR="$HOME/.termchan"
BIN_DIR="$HOME/.bin"
BINARY="$BIN_DIR/termchan"
CONFIG="$DIR/termchan.conf"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[termchan]${NC} $*"; }
warn()    { echo -e "${YELLOW}[termchan]${NC} $*"; }
die()     { echo -e "${RED}[termchan] error:${NC} $*" >&2; exit 1; }

echo ""
echo "  This will remove:"
echo "    binary  : $BINARY"
echo "    repo    : $DIR"
echo ""

REMOVE_CONFIG=0
if [ -f "$CONFIG" ]; then
    read -r -p "  Keep your config file (~/.termchan/termchan.conf)? [Y/n] " keep_cfg
    case "$keep_cfg" in
        [nN]*) REMOVE_CONFIG=1 ;;
        *)     REMOVE_CONFIG=0 ;;
    esac
fi

echo ""
read -r -p "  Proceed with uninstall? [y/N] " confirm
case "$confirm" in
    [yY]*) ;;
    *) warn "Aborted."; exit 0 ;;
esac

echo ""

if [ -f "$BINARY" ]; then
    rm -f "$BINARY"
    info "Removed binary: $BINARY"
else
    warn "Binary not found at $BINARY (already removed?)"
fi

if [ -d "$DIR" ]; then
    if [ "$REMOVE_CONFIG" -eq 0 ] && [ -f "$CONFIG" ]; then
        TMP_CONF=$(mktemp)
        cp "$CONFIG" "$TMP_CONF"
        rm -rf "$DIR"
        mkdir -p "$DIR"
        mv "$TMP_CONF" "$CONFIG"
        info "Removed repo: $DIR (config kept)"
    else
        rm -rf "$DIR"
        info "Removed repo and config: $DIR"
    fi
else
    warn "Repo not found at $DIR (already removed?)"
fi

if [ -d "$BIN_DIR" ] && [ -z "$(ls -A "$BIN_DIR")" ]; then
    rmdir "$BIN_DIR"
    info "Removed empty directory: $BIN_DIR"
fi

RC_PATTERN='export PATH="\$HOME/\.bin:\$PATH"'
for rc in "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.zshrc" "$HOME/.profile"; do
    if [ -f "$rc" ] && grep -qE "$RC_PATTERN" "$rc" 2>/dev/null; then
        # Only remove it if ~/.bin is now gone (don't break other tools)
        if [ ! -d "$BIN_DIR" ]; then
            sed -i "/export PATH=\"\$HOME\/.bin:\$PATH\"/d" "$rc"
            info "Removed PATH entry from $rc"
        fi
    fi
done

echo ""
info "termchan uninstalled."
