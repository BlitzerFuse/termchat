#!/bin/bash
set -e

REPO="git@github.com:BlitzerFuse/termchan.git"
DIR="$HOME/termchan"
BIN_DIR="$HOME/bin"

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

# inetutils provides `hostname -i`; already present on Fedora/RHEL
if command -v pacman &>/dev/null; then
  install_pkg inetutils
elif command -v apt &>/dev/null; then
  install_pkg inetutils-hostname
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

grep -qF 'export PATH="$HOME/bin:$PATH"' ~/.bashrc ||
  echo 'export PATH="$HOME/bin:$PATH"' >>~/.bashrc

echo "Opening firewall ports (5000/tcp, 5051/udp)..."
if command -v firewall-cmd &>/dev/null; then
  sudo firewall-cmd --add-port=5000/tcp --permanent
  sudo firewall-cmd --add-port=5051/udp --permanent
  sudo firewall-cmd --reload
elif command -v ufw &>/dev/null; then
  sudo ufw allow 5000/tcp
  sudo ufw allow 5051/udp
fi

echo ""
echo "termchan installed successfully!"
echo "run: source ~/.bashrc"
echo "type: termchan to open TUI"
