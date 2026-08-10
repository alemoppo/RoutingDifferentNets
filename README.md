# Network Route Manager

Windows desktop utility (C11 + SDL3) that pins specific IPv4 addresses — or all
IPs currently used by a given process — to a chosen network interface through
persistent route-table entries.

Typical use case: you have several networks (e.g. Wi-Fi + tethered phone) and
want certain destinations to always leave through a specific adapter, even
after a reboot or after the tether disconnects and reconnects.

## Features

- **One rule per IP** — add a single IPv4 address, or a process name: the app
  resolves every IP that process is connected to (TCP remote addresses, UDP
  local addresses) and creates one rule per resolved address.
- **Persistent, self-healing routes** — routes are created with
  `route.exe -p` (persistent across reboots) and reinforced with the native
  `CreateIpForwardEntry2` API when they do not show up in the routing table.
- **Stable adapter identity** — rules store the adapter **GUID**, never the
  ifIndex or the local IPv4. The current ifIndex/gateway are resolved
  dynamically at every snapshot, so rules survive adapter renumbering.
- **Event-driven monitoring** — network changes are detected with
  `NotifyIpInterfaceChange` / `NotifyUnicastIpAddressChange` /
  `NotifyRouteChange2` (no polling, ~0% CPU at idle). On any change the app
  re-reconciles and re-creates missing routes automatically, tolerating
  tether disconnect/reconnect.
- **REFRESH** — recalculates the tables and also **adopts existing manual
  host routes** (e.g. added via `route add` / `New-NetRoute`) whose next hop
  matches the gateway of a managed interface.
- **APPLY** — corrects conflicting routes and re-creates missing ones using
  current network parameters.
- **Status panel** — per-rule status: `OK`, `OFFLINE`, `MISSING`, `WRONG`,
  `ERROR`, plus a summary with counts.
- **Safe execution** — `route.exe` is spawned with `CreateProcessW`
  (`CREATE_NO_WINDOW`), never through `cmd.exe`/`system()`; all variable
  arguments are validated IPv4 addresses, so no shell injection is possible.

## Requirements

- Windows 10 / 11 (administrator rights — the app self-elevates via UAC
  manifest; routing table changes require elevation).
- [MinGW-w64](https://www.mingw-w64.org/) toolchain (`gcc`, `windres`) — e.g.
  via [MSYS2](https://www.msys2.org/): `pacman -S mingw-w64-x86_64-gcc`.
- SDL3 and SDL3_ttf for MinGW, placed under `deps\`:

  ```
  deps\SDL3-3.4.14\x86_64-w64-mingw32\
  deps\SDL3_ttf-3.2.2\SDL3_ttf-3.2.2\x86_64-w64-mingw32\
  ```

  Download from https://github.com/libsdl-org/SDL/releases and
  https://github.com/libsdl-org/SDL_ttf/releases.

## Building

Two equivalent build paths:

### build.bat (simple)

```
build.bat
```

Compiles each `src\*.c`, links `NetworkRouteManager.exe` and copies
`SDL3.dll` / `SDL3_ttf.dll` next to it if missing.

### CMake

```
cmake -G "MinGW Makefiles" -DMINGW=ON .
cmake --build .
```

The project requires the `MINGW` flag (set in toolchain files) and copies the
two DLLs next to the executable after the build.

## Usage

Run `NetworkRouteManager.exe` (double-click; UAC will ask for administrator
privileges). The window shows:

- **Left panel** — network interfaces (name, IPv4, gateway, ifIndex, metric,
  whether the default route passes through it) and the list of IPs assigned to
  each one, with per-IP `EDIT` / `REMOVE` buttons.
- **Right panel** — routing rules table (IP, configured interface, status)
  and a status summary. Buttons: `ADD IP`, `REFRESH`, `APPLY`.

### Adding a rule

1. Click **ADD IP**.
2. Type either:
   - a valid IPv4 address, or
   - a process name (e.g. `spotify`, with or without `.exe`) — the app will
     resolve its connected IPs and add one rule per address (max 8).
3. Select the target network interface from the dropdown and click **ADD**
   (or press Enter).

The dialog supports `Ctrl+V` (paste) and `Ctrl+C` (copy).

### Removing / editing

Use the `REMOVE` / `EDIT` buttons in the left panel, or remove a rule and
delete its route from the system table.

### Keyboard

| Key  | Action          |
|------|-----------------|
| F5   | Refresh + adopt |

### Configuration file

Rules are persisted in a minimal hand-written JSON file (no external
libraries):

```
%LOCALAPPDATA%\NetworkRouteManager\config.json
```

```json
{
  "routes": [
    { "ip": "37.244.28.101", "interface": "Ethernet 2", "guid": "{...}" }
  ]
}
```

Rules are identified by IP; the interface is stored as friendly name (for
display) + adapter GUID (as the persistent identity). Maximum 256 rules.

## Project layout

```
src/
  main.c        entry point, admin-rights check
  gui.c         SDL3 software-rendered UI, dialog, reconcile loop
  network.c     interface enumeration (GetAdaptersAddresses, metrics)
  routes.c      routing table snapshot + add/delete routes (CLI + native)
  config.c      JSON persistence (%LOCALAPPDATA%\...)
  monitor.c     event-driven change monitor (Notify* APIs, dedicated thread)
  proc.c        process name -> connected IPs resolution (TCP/UDP tables)
  resources.rc  UAC manifest + version info
build.bat       MinGW build script
CMakeLists.txt  alternative CMake build
```

### Console test tool

`src\backend_test.c` is a small console harness for the backend (no GUI):

```
gcc -O2 -o backend_test.exe src\backend_test.c src\network.c src\routes.c src\config.c -liphlpapi

backend_test.exe             -> interfaces + routes
backend_test.exe routes      -> routing table only
backend_test.exe config <f>  -> show rules saved in <f>
```

## Limitations

- Windows only, IPv4 only.
- Administrator privileges required.
- The default route (0.0.0.0/0) is never touched automatically; it is only
  displayed for reference.