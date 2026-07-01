# GUI Demo

The GUI is an early Tauri desktop demo for the v2 direction. It keeps the
existing C++ diagnostic binary as the backend command source and renders
Diagnostics and Logs views in a desktop shell.

The visual direction is a dense desktop diagnostics dashboard. The React layer
uses bordered panes, status fields, compact controls, and a log viewport instead
of embedding a terminal UI library.

## Prerequisites

- Rust and Cargo
- Node.js and pnpm
- A built `livox_mid360_diagnostics` binary
- Tauri Linux WebView development packages when running on Linux

The desktop scaffold intentionally uses Tauri 1.x so Ubuntu 20.04 can remain a
supported GUI development target. Tauri 2 currently pulls the newer
WebKitGTK 4.1 / libsoup3 stack, which is a poor fit for the existing Ubuntu
20.04 baseline.

Ubuntu 20.04:

```bash
sudo apt install -y libwebkit2gtk-4.0-dev libgtk-3-dev libappindicator3-dev librsvg2-dev libsoup2.4-dev
```

Ubuntu 22.04+ can usually use the same Tauri 1 dependency set:

```bash
sudo apt install -y libwebkit2gtk-4.0-dev libgtk-3-dev libappindicator3-dev librsvg2-dev libsoup2.4-dev
```

The GUI searches for the binary in this order:

1. `LIVOX_MID360_DIAGNOSTICS_BIN`
2. the bundled app resource directory
3. the installed executable directory
4. `build/sdk2/livox_mid360_diagnostics`
5. `build/livox_mid360_diagnostics`
6. repository root
7. `dist/prebuilt`

On Linux it also checks the current architecture-specific prebuilt name under
`dist/prebuilt`, such as `livox_mid360_diagnostics-linux-x86_64`.

GitHub Release assets include Linux x86_64/aarch64 GUI packages named like
`livox-mid360-diagnostics-gui-<version>-linux-<arch>.AppImage` and `.deb`, with
`.rpm` included when the bundler produces it. Release GUI packages embed the
matching CLI backend binary as an app resource.

Build the backend binary first when needed:

```bash
./scripts/build_cpp_with_sdk2.sh
```

## Run

```bash
cd gui
pnpm install
pnpm tauri dev
```

To preview just the web UI while system WebView dependencies are not installed:

```bash
cd gui
pnpm dev
```

The preview mode only renders the React interface. Running Diagnostics from the
button requires the Tauri shell because that action calls Rust commands.

Use the toolbar fields to set an interface, discovery timeout, and auto-bind
options. The Diagnostics button runs the current C++ `monitor` entry point
continuously, parses device, thermal, stream, network, firmware, state, and SDK
detail fields into the dashboard, and supports pause, resume, and stop from the
current panel. Details only shows fields that are actually available from the
current run; raw command output remains available in the Logs view.

If the binary lives somewhere else:

```bash
LIVOX_MID360_DIAGNOSTICS_BIN=/absolute/path/to/livox_mid360_diagnostics pnpm tauri dev
```
