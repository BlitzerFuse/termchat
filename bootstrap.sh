#!/bin/bash

set -e
REPO="git@github.com:BlitzerFuse/termchat.git"
DIR="$HOME/termchat"
BIN_DIR="$HOME/bin"

echo "Starting termchat bootstrap..."
if command -v pacman &>/dev/null; then
  PKG_MANAGER="pacman"
elif command -v apt &>/dev/null; then
  PKG_MANAGER="apt"
else
  echo "Unsupported Linux distribution. Please install git, make, and gcc manually."
  exit 1
fi

echo "Checking dependencies..."

for pkg in git make gcc; do
  if ! command -v $pkg &>/dev/null; then
    echo "$pkg not found, installing..."
    if [ "$PKG_MANAGER" = "pacman" ]; then
      sudo pacman -Syu --noconfirm $pkg
    elif [ "$PKG_MANAGER" = "apt" ]; then
      sudo apt update
      sudo apt install -y $pkg
    fi
  else
    echo "$pkg found."
  fi
done

if [ ! -d "$DIR" ]; then
  echo "Cloning termchat repo..."
  git clone "$REPO" "$DIR"
else
  echo "Repo already exists, pulling latest changes..."
  cd "$DIR"
  git pull origin main
fi

cd "$DIR"
make

mkdir -p "$BIN_DIR"
cp termchat "$BIN_DIR/"

if ! grep -q 'export PATH="$HOME/bin:$PATH"' ~/.bashrc; then
  echo 'export PATH="$HOME/bin:$PATH"' >>~/.bashrc
fi

echo ""
echo "termchat installed successfully!"
echo "Restart your terminal or run: source ~/.bashrc"
echo "You can now run termchat using:"
echo "   termchat listen        # to wait for connection"
echo "   termchat connect <ip>  # to connect to a peer"
