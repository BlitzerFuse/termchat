#!/bin/bash
set -e

REPO="https://github.com/BlitzerFuse/termchan"
DIR="$HOME/.termchan"
BIN_DIR="$HOME/.bin"

install_pkg() {
  if command -v pacman &>/dev/null; then
    sudo pacman -S --noconfirm "$@"
  elif command -v apt &>/dev/null; then
    sudo apt install -y "$@"
  elif command -v dnf &>/dev/null; then
    sudo dnf install -y "$@"
  else
    echo "Unsupported package manager. Install manually: $*"
    exit 1
  fi
}

echo "Checking dependencies..."
for pkg in git make gcc; do
  command -v "$pkg" &>/dev/null || install_pkg "$pkg"
done

if command -v pacman &>/dev/null; then
  install_pkg ncurses
else
  install_pkg libncurses-dev 2>/dev/null || install_pkg ncurses-devel
fi

if [ -d "$DIR" ]; then
  git -C "$DIR" pull origin main
else
  git clone "$REPO" "$DIR"
fi

echo "Building termchan..."
if ! make -C "$DIR"; then
  echo "Build failed. Check the output above for errors."
  exit 1
fi

if [ ! -f "$DIR/termchan" ]; then
  echo "Binary not found after build. Something went wrong."
  exit 1
fi

mkdir -p "$BIN_DIR"
cp "$DIR/termchan" "$BIN_DIR/"

# FIX L-2: corrected path from "$HOME/bin" to "$HOME/.bin" — the binary
# is installed into $HOME/.bin (with the dot prefix) so the PATH export
# must reference the same directory, otherwise 'termchan' is never found
# after running 'source ~/.bashrc'.
grep -qF 'export PATH="$HOME/.bin:$PATH"' ~/.bashrc ||
  echo 'export PATH="$HOME/.bin:$PATH"' >>~/.bashrc

echo ""
echo "termchan installed. Run: source ~/.bashrc && termchan"
