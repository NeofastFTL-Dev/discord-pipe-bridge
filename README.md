# discord-pipe-bridge

Let Windows apps running under **Wine** talk to the **native Linux Discord** client.

Windows programs open the named pipe `\\.\pipe\discord-ipc-0` to set Rich Presence. Discord on
Linux does not have such a pipe — it listens on the unix socket
`$XDG_RUNTIME_DIR/discord-ipc-0` — and a Wine process cannot reach that socket itself: Wine's
`ws2_32` does not implement `AF_UNIX` (`socket()` fails with `WSAEAFNOSUPPORT`, 10047, on
wine-11.22).

A binary built with `winegcc` *can* call the real POSIX `socket()`/`connect()`, because the unix
half of the program runs against libc. This project is that program: one small process inside the
prefix that serves the pipe and relays every byte to Discord.

## How it works

```
Wine app ──► \\.\pipe\discord-ipc-0 ──► discord-pipe-bridge ──► $XDG_RUNTIME_DIR/discord-ipc-0 ──► Discord
```

* The relay is **byte-transparent** — Discord's framing (8-byte op/length header + JSON) is passed
  through untouched, so the handshake, `SET_ACTIVITY` and Discord's replies all work unmodified.
* One pipe client maps to one Discord connection. When the client disconnects, the bridge drops
  the socket and waits for the next client, so both the app and Discord can restart freely.
* The Discord socket is auto-detected: native (`$XDG_RUNTIME_DIR/discord-ipc-*`), Flatpak
  (`app/com.discordapp.Discord`), Snap (`snap.discord`, `snap.discord-canary`) and Vesktop.
* A named mutex makes a second instance exit immediately, so it is safe to start on every launch.
* If Discord is not running yet, the client is disconnected promptly instead of hanging, and can
  retry on its own.

## Requirements

* Wine with the development tools — `winegcc` (package `wine-devel`, `wine-staging` or equivalent)
  and `gcc`. Tested with **wine-11.22** on CachyOS.
* Any native Discord client that creates a unix socket in `$XDG_RUNTIME_DIR`.

## Build

```bash
./build.sh          # winegcc -O2 -Wall -Wextra → dist/discord-pipe-bridge.exe (+ .exe.so)
```

`build.sh` is part of this repository, so a fresh clone builds without any helper from outside it;
the only external requirement is `winegcc` (package `wine-devel`/`wine-gcc` depending on distro).

Two files are produced and **must stay together** — the PE side and the unix side that performs
the actual socket connection:

```
dist/discord-pipe-bridge.exe
dist/discord-pipe-bridge.exe.so
```

## Install and run

Inside the prefix where your Windows app runs (anything works, this is not app-specific):

```bash
export WINEPREFIX="$HOME/.wine-apps"
install -m755 dist/discord-pipe-bridge.exe    "$WINEPREFIX/drive_c/windows/"
install -m644 dist/discord-pipe-bridge.exe.so "$WINEPREFIX/drive_c/windows/"

wine 'C:\windows\discord-pipe-bridge.exe' &
```

Start the bridge **before** the app that wants presence (or let a launcher do it, see below).
Logs go to stderr; redirect them somewhere when running in the background.

### From a launcher script

```bash
start_bridge() {
  local exe="$WINEPREFIX/drive_c/windows/discord-pipe-bridge.exe"
  [ -f "$exe" ] || return 0
  wine "$exe" >>"$HOME/.local/state/neo-apps/discord-pipe-bridge.log" 2>&1 &
}
```

The second and later copies notice the first one and exit, so this is safe to call every launch.

## Options

| Variable | Effect |
|---|---|
| `DISCORD_IPC_SOCKET` | Use this socket path instead of auto-detection |
| `BRIDGE_DEBUG=1` | Dump relayed bytes to the log (readable JSON between frames) |
| `XDG_RUNTIME_DIR` | Base directory for autodetection (falls back to `/run/user/<uid>`) |

## Confirming it works

With `BRIDGE_DEBUG=1`, a healthy session looks like this:

```
[12:35:58] serving \\.\pipe\discord-ipc-0 for the native Linux Discord socket
[12:36:02] client connected on \\.\pipe\discord-ipc-0
[12:36:02] connected to /run/user/1000/discord-ipc-0
    pipe->discord 49:....)...{"v":1,"client_id":"…"}
    discord->pipe 417:........{"cmd":"DISPATCH","data":{…"username":"…"…},"evt":"READY","nonce":null}
    pipe->discord 171:........{"cmd":"SET_ACTIVITY","args":{…"activity":{…}},"nonce":"…"}
    discord->pipe 253:........{"cmd":"SET_ACTIVITY","data":{"type":2,…},"evt":null,"nonce":"…"}
```

`READY` carrying your own username means the real client answered; a `SET_ACTIVITY` reply that
echoes the activity is Discord having accepted it.

## Limitations

* Only `discord-ipc-0` is served (that is the pipe every normal client looks for). Clients that
  insist on `discord-ipc-1…9` are not covered.
* One client at a time; a second concurrent client waits for the pipe instance instead of sharing
  the socket.
* The bridge must run in the **same prefix** as the app — Wine named pipes do not cross prefixes.
* Windows-side TLS/HTTP is untouched: this is only the local RPC pipe.

## Screenshots

There is no window — the log excerpt above is the visible output. If screenshots are added later,
put them in `docs/screenshots/`: one of the bridge log next to Discord showing the same activity,
and one of the app it serves.

## Credits

* Prior art and protocol reference:
  [0e4ef622/wine-discord-ipc-bridge](https://github.com/0e4ef622/wine-discord-ipc-bridge) (MIT) and
  [openglfreak/winestreamproxy](https://github.com/openglfreak/winestreamproxy) (MPL-2.0).
* The [Wine](https://www.winehq.org/) project — `winegcc`, and the named-pipe implementation this
  relies on.
* Discord's local RPC protocol, as implemented by the official client.

## License

No license file yet — one will be added before the first public release. Until then the code is
“all rights reserved” by default.
