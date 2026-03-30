# term-chan

A LAN / WiFi chat application for the terminal, written in C. No internet, no server, no accounts — just people on the same network.

---

## Features

- **Automatic peer discovery** — broadcasts a UDP beacon every 500ms; peers appear in the connect screen live as they launch the app
- **Multi-user rooms** — host creates a room, peers join, host starts when ready (up to 7 peers)
- **Optional password protection** — none, auto-generated, or manually set; shown in the room lobby
- **Accept / reject connections** — host approves each incoming peer individually
- **ncurses TUI** — full-terminal interface with scrollback, input bar, and status messages
- **Slash commands** — see [Commands](#commands) below
- **Config file** — saves your nickname and ports to `~/.termchan/termchan.conf`
- **Configurable port** — set in the config, in the menu, or with `-p` at launch
- **Runtime firewall** — automatically opens and closes ports in ufw or firewalld for the duration of the session

---

## Dependencies

| Package | Arch | Debian/Ubuntu | Fedora |
|---|---|---|---|
| gcc | `gcc` | `gcc` | `gcc` |
| make | `make` | `make` | `make` |
| ncurses | `ncurses` | `libncurses-dev` | `ncurses-devel` |

---

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/BlitzerFuse/termchan/main/scripts/bootstrap.sh | bash
source ~/.bashrc
```

Or manually:

```bash
git clone git@github.com:BlitzerFuse/termchan.git
cd termchan
make
./termchan
```

---

## Usage

```
termchan [-p <port>]
```

| Flag | Default | Description |
|---|---|---|
| `-p`, `--port` | `5000` | TCP port to listen or connect on |
| `-h`, `--help` | — | Print usage |

Running two instances on the same machine requires different ports:

```bash
./termchan -p 5000   # terminal 1
./termchan -p 5001   # terminal 2
```

---

## How it works

### Create Room

Select **Create Room** from the main menu, choose a password (or none), and you are dropped straight into the room lobby. The lobby shows:

- Your nickname and password (or "none")
- A live list of connected peers and how many slots remain
- Controls: `Enter = start   q = quit   (waiting for peers...)`

As people connect you see an accept/reject prompt for each one. You can start the chat at any time — even with an empty room — by pressing Enter. All connected peers receive a start signal and enter the chat simultaneously.

### Connect

Select **Connect** from the main menu. The peer list populates live as beacons arrive from other running instances. Navigate with arrow keys and press Enter to select a peer, press `r` to clear and wait for fresh beacons, or press `Tab`/`i` to type an IP manually.

After connecting you will see a holding screen while waiting for the host to start the session.

---

## Commands

| Command | Description |
|---|---|
| `/help` | List all commands |
| `/nick <n>` | Change your nickname — notifies all peers immediately |
| `/me` | Show your nickname, IP, role (host/guest), and peer count |
| `/ip` | Show your local IP address |
| `/reply <msg>` | Reply to the last person who sent a message (prefixes `@nick`) |
| `/pass` | Show the session password — host only |
| `/clear` | Clear the chat window |
| `/quit` | Leave the session |

---

## Config

The config file lives at `~/.termchan/termchan.conf` and is created automatically on first run. Edit it with any text editor:

```ini
# term-chan configuration
# Lines starting with # are comments.

nickname       = yourname
port           = 5000
discovery_port = 5051
```

| Key | Default | Description |
|---|---|---|
| `nickname` | *(empty)* | Pre-fills the nickname field in the menu |
| `port` | `5000` | Default TCP chat port |
| `discovery_port` | `5051` | UDP port used for peer discovery beacons |

Settings are saved back automatically when you exit the menu, so whatever you last used becomes the new default.

**Priority order:** config file → menu edits → `-p` flag (flag takes highest precedence).

---

## Project structure

```
termchan/
├── include/
│   ├── protocol.h       # Packet struct, MsgType enum
│   ├── session.h        # Session (peers, fds, nicks, password)
│   ├── chat.h           # start_chat()
│   ├── room.h           # room_broadcast / add / remove / shutdown
│   ├── network.h        # TCP connect / accept / handshake
│   ├── discovery.h      # UDP beacon API
│   ├── commands.h       # Command dispatch
│   ├── config.h         # Config load / save / defaults
│   ├── firewall.h       # Runtime firewall open / close
│   ├── tui.h            # Public TUI API
│   └── tui_internal.h   # TUI-internal helpers
├── src/
│   ├── main.c           # Entry point, arg parsing, session setup
│   ├── config.c         # Config file read/write
│   ├── termchan.conf    # Default config (reference copy)
│   ├── chat/
│   │   ├── chat.c       # Per-peer recv threads, input loop
│   │   ├── commands.c   # /command handlers
│   │   └── room.c       # Peer list with mutex-safe broadcast
│   ├── network/
│   │   ├── network.c    # TCP listener, connect, handshake packets
│   │   ├── discovery.c  # UDP beacon thread and peer table
│   │   └── firewall.c   # ufw / firewalld integration
│   └── tui/
│       ├── tui.c        # ncurses init, locale, resize signal
│       ├── tui_menu.c   # Main menu, peer list, waiting screens
│       ├── tui_chat.c   # Chat window, input bar, message display
│       └── tui_lobby.c  # Room lobby (host view)
├── scripts/
│   └── bootstrap.sh     # Install deps, clone, build, install binary
├── LICENSE
├── README.md
└── Makefile
```

---

## Ports

| Port | Protocol | Purpose |
|---|---|---|
| 5000 (default) | TCP | Chat connections |
| 5051 (default) | UDP | Peer discovery beacons |

If you have ufw or firewalld running, term-chan opens these ports automatically when a room is created and closes them when the session ends. If neither firewall is active nothing special is needed.

---

## Notes

- Both peers must be on the same local network
- Nicknames cannot contain spaces — they are replaced with underscores automatically
- Passwords are 6 characters, A-Z and 0-9 only
- A room supports up to 7 peers plus the host
- Changing your nickname with `/nick` notifies all peers in real time

## NOTICE

Many parts of the project WERE made in fact with AI. I'm not saying that the project is pure AI but without it, the project would probably be worse in my opinion. Take that as you want, I'll continue using AI for this project until I have perfected my skills in C (<- will probably never happen).
