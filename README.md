```
╔══════════════════════════════════════════════════════════════════════╗
║                                                                      ║
║   ██████╗  ██████╗  ██████╗██╗  ██╗███████╗████████╗                 ║
║   ██╔══██╗██╔═══██╗██╔════╝██║ ██╔╝██╔════╝╚══██╔══╝                 ║
║   ██████╔╝██║   ██║██║     █████╔╝ █████╗     ██║                    ║
║   ██╔═══╝ ██║   ██║██║     ██╔═██╗ ██╔══╝     ██║                    ║
║   ██║     ╚██████╔╝╚██████╗██║  ██╗███████╗   ██║                    ║
║   ╚═╝      ╚═════╝  ╚═════╝╚═╝  ╚═╝╚══════╝   ╚═╝                    ║
║                                                                      ║
║    ███████╗███████╗██╗  ██╗    ████████╗███████╗██████╗ ███╗   ███╗  ║
║    ██╔════╝██╔════╝██║  ██║    ╚══██╔══╝██╔════╝██╔══██╗████╗ ████║  ║
║    ███████╗███████╗███████║       ██║   █████╗  ██████╔╝██╔████╔██║  ║
║    ╚════██║╚════██║██╔══██║       ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║  ║
║    ███████║███████║██║  ██║       ██║   ███████╗██║  ██║██║ ╚═╝ ██║  ║
║    ╚══════╝╚══════╝╚═╝  ╚═╝       ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝  ║
║                                                                      ║
╚══════════════════════════════════════════════════════════════════════╝
```
<div align="center">
  <img src="misc/pipboy.gif" alt="PocketSSH Pip-Boy CRT Experience">
  <p><em>PocketSSH — Portable Handheld SSH Terminal for ESP32-S3 T-Deck Plus</em></p>
  <p><a href="https://youtu.be/iMoiEmd5Mq8">📺 Watch Demo Video</a></p>
</div>

# PocketSSH

PocketSSH is a standalone, ultra-portable SSH2 client and terminal environment engineered specifically for the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3). Featuring a retro Pip-Boy amber/green CRT aesthetic, PocketSSH provides a full-fledged interactive terminal experience with physical keyboard entry, dual-mode trackball navigation, touch gesture controls, MicroSD-backed public key management, remote key fetching, and a 25-test host Verification & Validation (V&V) suite.

![PocketSSH Screenshot](misc/screenshot_01.jpeg)

---

## Table of Contents

- [Key Features](#key-features)
  - [1. Interactive Desktop SSH Terminal](#1-interactive-desktop-ssh-terminal)
  - [2. Dual-Mode Shell: Local Shell vs. Remote Interactive SSH](#2-dual-mode-shell-local-shell-vs-remote-interactive-ssh)
  - [3. Dynamic Alt Mode & Trackball Navigation](#3-dynamic-alt-mode--trackball-navigation)
  - [4. Missing Symbols & Control Key Shortcuts](#4-missing-symbols--control-key-shortcuts)
  - [5. SSH Key Authentication & Remote Key Fetching](#5-ssh-key-authentication--remote-key-fetching)
  - [6. Full ANSI 256-Color & TrueColor Rendering](#6-full-ansi-256-color--truecolor-rendering)
  - [7. Smart Emoji Engine & UTF-8 Stream Parser](#7-smart-emoji-engine--utf-8-stream-parser)
  - [8. Robust 2.4 GHz WiFi Subsystem & Persistence](#8-robust-24-ghz-wifi-subsystem--persistence)
  - [9. Deep Terminal Scrollback & Backlog Navigation](#9-deep-terminal-scrollback--backlog-navigation)
  - [10. Slide-Out Quick Keys Touch Panel](#10-slide-out-quick-keys-touch-panel)
  - [11. Pre-Build Verification & Validation (V&V) Suite](#11-pre-build-verification--validation-vv-suite)
- [Hardware Requirements & Pinout](#hardware-requirements--pinout)
- [Quick Start Guide](#quick-start-guide)
  - [1. WiFi Connection](#1-wifi-connection)
  - [2. SSH Connection](#2-ssh-connection)
  - [3. Toggling the Local Shell](#3-toggling-the-local-shell)
- [Command Reference](#command-reference)
  - [Local Shell Commands](#local-shell-commands)
  - [SSH Escape Commands](#ssh-escape-commands)
- [Keyboard & Trackball Cheat Sheet](#keyboard--trackball-cheat-sheet)
  - [Trackball Actions](#trackball-actions)
  - [Alt Mode Missing Symbols](#alt-mode-missing-symbols)
  - [Alt Mode Control Keys](#alt-mode-control-keys)
- [SSH Key Management Guide](#ssh-key-management-guide)
  - [PEM Format Requirement](#pem-format-requirement)
  - [Generating & Converting Keys](#generating--converting-keys)
  - [Remote Key Fetching (`getkey` / `:getkey`)](#remote-key-fetching-getkey--getkey)
  - [MicroSD Directory Structure](#microsd-directory-structure)
- [Verification & Validation (V&V) Guide](#verification--validation-vv-guide)
  - [Host Pre-Build Unit & Integration Tests](#host-pre-build-unit--integration-tests)
  - [Virtual ESP32-S3 Target (QEMU) Integration Tests](#virtual-esp32-s3-target-qemu-integration-tests)
- [Architecture & Technical Details](#architecture--technical-details)
  - [Shared SPI Bus Architecture](#shared-spi-bus-architecture)
  - [FreeRTOS Tasks](#freertos-tasks)
  - [Display Performance & Batching](#display-performance--batching)
- [Building & Flashing](#building--flashing)
  - [Pre-Built Merged Binary (Offset 0x0)](#pre-built-merged-binary-offset-0x0)
  - [Building from Source](#building-from-source)
- [Version History](#version-history)
- [Troubleshooting](#troubleshooting)
- [License & Credits](#license--credits)

---

## Key Features

### 1. Interactive Desktop SSH Terminal
- **Full-Screen 53x24 Grid**: When an SSH session opens, the local input bar collapses automatically, expanding the terminal viewport to 53 columns by 24 rows of text.
- **Blinking Green Cursor**: True visual cursor with non-destructive horizontal positioning and auto-scroll to prompt on connect (no blank prompt waiting for Enter).
- **Line Discipline & Word Wrapping**: Distinct carriage return (`\r`) and linefeed (`\n`) handlers ensure commands longer than a single line wrap smoothly to the next row without overwriting or disappearing from previous lines.
- **Destructive Backspace Clamping**: Implements Unix `VERASE=127` and `\b \b` erasure while preventing destructive backspacing past the start of the remote prompt.
- **Pure Tilde (`~/`) Passthrough**: Tilde (`~`) is completely deconflicted and passes straight to the remote shell with zero latency—no escape buffering, no delay, and no double-echoing when typing `cd ~/`, `cat ~/.ssh/id_rsa`, or `~/script.sh`.
- **High-Speed Stream Batching**: Terminal output streams are flushed in 256-byte chunks, reducing LVGL display lock contention by 1000x and eliminating hangs during large directory listings or file dumps.
- **Real-Time Activity HUD**: Status pill in the top-right displays `[WAIT...]` on command transmission, an animated rotating spinner `[ / ]` `[ - ]` `[ \ ]` `[ | ]` during incoming byte streams, and `[ONLINE]` when idle.

### 2. Dual-Mode Shell: Local Shell vs. Remote Interactive SSH
- **Alt + Enter Toggle**: Seamlessly toggle between **Full Interactive SSH Mode** and **PocketSSH Local Shell Mode** (`[local]> `) at any time without disconnecting your active SSH session.
- **Protected Local Execution**: In local mode, type commands like `getkey`, `keys`, `test`, `status`, or `help` with full local cursor navigation and history editing; nothing leaks to the remote server.
- **Seamless Resume**: Return to your active SSH session instantly by pressing **Alt + Enter** again or typing **`resume`**, **`fg`**, or **`ssh`**.
- **Touch Button**: A dedicated **`Local/SSH`** toggle button is also available in the slide-out touch Keys panel.

### 3. Dynamic Alt Mode & Trackball Navigation
- **Alt Mode Toggle**: Click the hardware trackball (short press) or tap the on-screen **`[ALT]`** touch pill to toggle Alt Mode on and off.
- **Dynamic On-Screen Alt HUD**: When Alt Mode is active, a floating yellow HUD banner appears across the top:
  ```text
  [ALT ON] Enter:Local Shell | Space:Tab | Trackball:Arrows
  ```
- **Dual-Behavior Trackball**:
  - **Normal Mode (Scroll)**: Rolling the trackball Up/Down scrolls backward and forward through terminal scrollback history. Clicking while scrolled immediately snaps back to the live prompt.
  - **Alt Mode (Arrow Keys)**: Rolling the trackball Up/Down/Left/Right transmits ANSI arrow sequences (`\033[A`, `\033[B`, `\033[D`, `\033[C`) to navigate command history and control cursors in `vim`, `nano`, `less`, and shell prompts.
  - **Long-Press SIGINT**: Holding the trackball down for >400 ms sends `Ctrl+C` (`\x03`) immediately in SSH mode to abort runaway processes.

### 4. Missing Symbols & Control Key Shortcuts
The LilyGO T-Deck hardware keyboard lacks several keys required for standard Unix shell interaction. PocketSSH maps all missing characters and control signals into intuitive single-key chords via Alt Mode:

- **Missing Characters**: `~`, `|`, `\`, `=`, `[`, `]`, `{`, `}`, `<`, `>`, `` ` ``, `$`, `^`, `&`, `%`, `/`.
- **Tab Auto-Completion**: Press **`Space`** while in Alt Mode (or `Alt + Space`) to send `Tab` (`\t`) for remote bash/zsh file and command completion.
- **Control Keys**: Instant chords for `Ctrl+C`, `Ctrl+Z`, `Ctrl+D`, `Ctrl+L`, `Ctrl+A`, `Ctrl+R`, `Ctrl+X`, and `Ctrl+W`.
- **Dedicated Touch Buttons**: Top-bar quick pills for **`[ALT]`**, **`[ESC]`**, **`[^C]`**, and **`[KEY]`**.

### 5. SSH Key Authentication & Remote Key Fetching
- **MicroSD Key Management**: Automatically loads all `.pem` RSA keys from `/sdcard/ssh_keys/` on boot into memory for fast public-key authentication (`sshkey`).
- **Interactive Remote Key Fetcher (`getkey` / `:getkey`)**:
  - Fetch private keys directly from connected SSH hosts into `/sdcard/ssh_keys/<name>.pem` and NVS flash storage.
  - **Interactive File Picker Dialog**: Running `getkey` without arguments opens a touch modal listing all `.pem` files discovered in the remote `~/.ssh/` directory.
  - **Automatic PEM Format Validation**: Enforces PKCS#1 PEM headers (`-----BEGIN RSA PRIVATE KEY-----`) and displays helpful diagnostic warnings if an OpenSSH format key is detected.
  - **Key Name Resolution**: Automatically resolves key names with or without the `.pem` extension (e.g., `id_rsa` resolves to `id_rsa.pem`).

### 6. Full ANSI 256-Color & TrueColor Rendering
- **16 Standard & Bright Colors**: Fully maps ANSI codes `30m-37m`, `40m-47m`, `90m-97m`, and `100m-107m`.
- **256-Color Palette**: Implements the full 6x6x6 xterm color cube (`\033[38;5;16m` to `231m`) and 24-step grayscale ramp (`232m` to `255m`).
- **24-Bit TrueColor**: Renders arbitrary RGB colors (`\033[38;2;r;g;bm` and `\033[48;2;r;g;bm`).
- **Syntax Isolation**: Escapes literal `#` characters in output streams to prevent interference with shell prompts, comments, or LVGL markup.

### 7. Smart Emoji Engine & UTF-8 Stream Parser
- **Network Packet Boundary Resilience**: Multi-byte UTF-8 stream decoder handles 1 to 4 byte UTF-8 sequences spanning fragmented TCP packets.
- **Automated Developer Emoji Translation**: Maps 65+ common Git, CI/CD, and DevOps emojis (`✅`, `❌`, `⚠️`, `ℹ️`, `🚀`, `🔥`, `📦`, `🐛`, `✨`, `🎉`, etc.) into compact, color-coded text badges.
- **Mojibake Elimination**: Gracefully normalizes unicode punctuation and renders unknown emojis as an unobtrusive `[?]` badge instead of breaking layout.

### 8. Robust 2.4 GHz WiFi Subsystem & Persistence
- **Interactive Network Scanner (`scan`)**: Scans all 2.4 GHz channels and lists visible networks sorted by RSSI signal strength with security types (Open, WEP, WPA, WPA2, WPA3, WPA2/WPA3).
- **Index-Based Connection**: Connect quickly by scan number: `connect 1 <password>`.
- **WPA3 Personal / SAE Support**: Enabled Hash-to-Element (H2E) and Hunting-and-Pecking (`WPA3_SAE_PWE_BOTH`).
- **Low-Latency Mode**: Modem power saving disabled (`WIFI_PS_NONE`) to ensure zero latency and prevent SSH connection drops.
- **Dual-Layer Credential Persistence**: Automatically saves credentials to ESP32 NVS flash and `/sdcard/wifi.cfg` / `/sdcard/wificonfig.json` with background auto-connect at boot.
- **Human-Readable Error Codes**: Reports specific connection failure reasons (e.g. `[201] AP not found`, `[202] Authentication failed`, `[15] Handshake timeout`).

### 9. Deep Terminal Scrollback & Backlog Navigation
- **16 KB Circular FIFO Buffer**: Large scrollback memory prevents sudden terminal wipes when running commands with large output.
- **Trackball & Touch Gestures**: Scroll line-by-line using the trackball or drag vertically on the screen with smooth momentum.
- **Visual Scrollbar**: Glowing green scroll indicator shows current position and scroll depth.
- **Instant Snap-to-Bottom**: Clicking the trackball or tapping the top `[^ SCROLL]` indicator immediately snaps back to the active prompt.

### 10. Slide-Out Quick Keys Touch Panel
- Swipe left-to-right (or tap the **`[KEY]`** pill) to open a slide-out panel with hardware shortcuts:
  - **Control Keys**: `Ctrl+C`, `Ctrl+Z`, `Ctrl+D`, `Ctrl+L`, `Tab`, `Esc`
  - **Scrollback Shortcuts**: `PgUp`, `PgDn`, `Top`, `Bottom`, `Clear`
  - **Session Management**: `Exit SSH`, `Local/SSH`, `Get Key`

### 11. Pre-Build Verification & Validation (V&V) Suite
- **28 Pre-Build Automated Tests**: Complete host test suite ([tests/test_terminal.py](tests/test_terminal.py) and [tests/run_tests.ps1](tests/run_tests.ps1)) runs before every build.
- **Zero-Tolerance Build Gate**: [build_firmware.ps1](build_firmware.ps1) immediately halts compilation if any unit or integration test fails.
- **Virtual / Host Simulation**: Rigorous behavioral verification covering ANSI color palettes, cursor clamping, line boundaries, escape sequences, Alt Mode mappings, and key parsing.

---

## Hardware Requirements & Pinout

PocketSSH is optimized for the **LilyGO T-Deck / T-Deck Plus**:

| Component | Interface / Pins | Details |
| :--- | :--- | :--- |
| **Microcontroller** | ESP32-S3 | 16 MB Flash, 8 MB PSRAM |
| **Display** | SPI (ST7789) | 320×240 IPS Color LCD, CS=GPIO 12, Shared `SPI2_HOST` |
| **MicroSD Card** | SPI | CS=GPIO 39, MISO=GPIO 38, Shared `SPI2_HOST` |
| **Touch Controller** | I2C (GT911) | Capacitive touch with gesture support |
| **Keyboard** | I2C (C3 Module) | Address `0x55`, full QWERTY layout |
| **Trackball** | GPIO Interrupts | Up=GPIO 1, Left=GPIO 2, Down=GPIO 3, Right=GPIO 15 |
| **Battery Monitor** | ADC1 Channel 3 | GPIO 4 with voltage divider |

---

## Quick Start Guide

### 1. WiFi Connection

Scan for available 2.4 GHz wireless networks:
```text
scan
```
Connect using the network's scan index number:
```text
connect 1 MyPassword123
```
Or connect directly by SSID (use double quotes if the SSID contains spaces):
```text
connect "Office WiFi" "SuperSecret123"
```
Check connection details and IP address:
```text
status
```

### 2. SSH Connection

**Password Authentication**:
```text
ssh <HOST> <PORT> <USER> <PASSWORD>
```
*Example:* `ssh 192.168.1.100 22 pi raspberry`

**Public Key Authentication**:
```text
sshkey <HOST> <PORT> <USER> <KEYFILE>
```
*Example:* `sshkey 192.168.1.100 22 pi id_rsa.pem`  
*(Or simplified syntax:* `sshkey pi@192.168.1.100 id_rsa.pem`*)*

### 3. Toggling the Local Shell

Once connected to an SSH server:
1. Press **Alt** (yellow HUD appears: `[ALT ON] Enter:Local Shell | Space:Tab | Trackball:Arrows`).
2. Press **Enter**.
3. The prompt switches to `[local]> ` without terminating your SSH session.
4. Run local commands like `getkey`, `keys`, `test`, or `status`.
5. Return to SSH by typing `resume` or pressing **Alt + Enter** again.

---

## Command Reference

### Local Shell Commands

These commands execute on the device firmware (in idle mode or inside the `[local]> ` shell during active SSH):

| Command | Syntax | Description |
| :--- | :--- | :--- |
| **`scan`** | `scan` or `wifi scan` | Scan 2.4 GHz WiFi networks sorted by RSSI |
| **`connect`** | `connect [SSID\|#] [PASS]` | Connect to WiFi network (or saved network if empty) |
| **`save`** | `save <SSID\|#> [PASS]` | Save WiFi credentials to NVS without connecting |
| **`saved`** | `saved` | Display currently saved WiFi credentials |
| **`forget`** | `forget` | Erase saved WiFi credentials from NVS |
| **`autoconnect`** | `autoconnect <on\|off>` | Toggle automated background WiFi connect on boot |
| **`status`** | `status` | Show IP, gateway, subnet, DNS, RSSI, and MAC address |
| **`disconnect`** | `disconnect` | Disconnect active WiFi connection |
| **`ssh`** | `ssh <HOST> [PORT] <USER> <PASS>` | Connect to SSH host using password auth |
| **`sshkey`** | `sshkey <H> [P] <U> <KEYFILE>` | Connect to SSH host using MicroSD/NVS private key |
| **`keys`** | `keys` or `key` | List all loaded SSH private keys |
| **`getkey`** | `getkey [REMOTE_PATH] [NAME]` | Fetch private key from remote server (opens picker if empty) |
| **`resume`** | `resume` or `fg` | Resume active SSH session from Local Shell mode |
| **`clear`** | `clear` | Clear the terminal display output |
| **`exit`** | `exit` | Terminate and disconnect the active SSH session |
| **`pgup`** / **`pgdn`** | `pgup` / `pgdn` | Scroll terminal backlog one page up / down |
| **`top`** / **`bottom`** | `top` / `bottom` | Jump to very beginning / end of scrollback buffer |
| **`scroll`** | `scroll` | Display cheat sheet of scrollback navigation controls |
| **`version`** | `version` or `ver` | Display PocketSSH build date, IDF version, and features |
| **`help`** | `help` | Display command help and usage syntax |

### SSH Escape Commands

When connected in interactive SSH mode, typing `:` as the very first character of a new line initiates an escape command without sending keystrokes to the remote shell:

| Escape Command | Description |
| :--- | :--- |
| **`:getkey [path] [name]`** | Pull private key from remote host, save to SD card & NVS |
| **`:keys`** | Print list of loaded private keys in memory |
| **`:exit`** | Close remote channel and disconnect SSH session |

---

## Keyboard & Trackball Cheat Sheet

### Trackball Actions

| Hardware State | Gesture / Action | Resulting Behavior |
| :--- | :--- | :--- |
| **Normal Mode** | Roll Up / Down | Scroll backward / forward through terminal output |
| **Normal Mode** | Short Click | Snap immediately to live prompt (if scrolled back) |
| **Normal Mode** | Roll Left / Right | Move local cursor left/right or cycle command history |
| **Any Mode** | Short Click (at bottom) | Toggle **Alt Mode** ON / OFF |
| **Alt Mode** | Roll Up / Down | Send ANSI `Up` / `Down` arrows (`\033[A`, `\033[B`) |
| **Alt Mode** | Roll Left / Right | Send ANSI `Left` / `Right` arrows (`\033[D`, `\033[C`) |
| **SSH Active** | Long Press (>400 ms) | Send `Ctrl+C` (`\x03` / SIGINT) immediately |

### Alt Mode Missing Symbols

Activate Alt Mode (trackball click or **`[ALT]`** touch pill), then press the corresponding key:

| Desired Symbol | Alt Mode Key | Description / Typical Unix Usage |
| :---: | :---: | :--- |
| **`~`** | **`t`** or **`T`** | Tilde (User home directory: `cd ~/`) |
| **`\|`** | **`p`** or **`P`** | Pipe (Command piping: `cat log \| grep err`) |
| **`\`** | **`b`** or **`B`** | Backslash (Escaping: `mkdir test\ dir`) |
| **`=`** | **`e`** or **`E`** | Equals (Assignments: `VAR=123`) |
| **`[`** | **`u`** or **`U`** | Left square bracket (Array indexing / test) |
| **`]`** | **`i`** or **`I`** | Right square bracket |
| **`{`** | **`o`** or **`O`** | Left curly brace (Shell expansions: `{a,b}`) |
| **`}`** | **`j`** or **`J`** | Right curly brace |
| **`<`** | **`k`** or **`K`** | Less than (Input redirection: `cmd < file`) |
| **`>`** | **`m`** or **`M`** | Greater than (Output redirection: `cmd > file`) |
| **``` ` ```** | **`q`** or **`Q`** | Backtick (Command substitution: ``` `date` ```) |
| **`$`** | **`s`** or **`S`** | Dollar sign (Variables: `$HOME`, `$PATH`) |
| **`^`** | **`g`** or **`G`** | Caret (Regex / history substitution) |
| **`&`** | **`n`** or **`N`** | Ampersand (Background jobs: `cmd &`) |
| **`%`** | **`h`** or **`H`** | Percent (Jobs / format strings) |
| **`/`** | **`f`** or **`F`** | Forward slash (Directory paths) |
| **`Tab`** | **`Space`** | Tab auto-completion (`\t`) |

### Alt Mode Control Keys

| Control Signal | Alt Mode Key | Description |
| :---: | :---: | :--- |
| **`Ctrl+C`** | **`c`** or physical **`Alt+C`** | SIGINT (Interrupt / abort current process) |
| **`Ctrl+Z`** | **`z`** | SIGTSTP (Suspend current foreground process) |
| **`Ctrl+D`** | **`d`** | EOF / Logout |
| **`Ctrl+L`** | **`l`** | Clear screen buffer |
| **`Ctrl+A`** | **`a`** | Beginning of line (Home) |
| **`Ctrl+R`** | **`r`** | Reverse history search in bash/zsh |
| **`Ctrl+X`** | **`x`** | Exit text editor (nano) |
| **`Ctrl+W`** | **`w`** | Delete preceding word |

---

## SSH Key Management Guide

PocketSSH uses `libssh2` with `mbedTLS`, requiring **PKCS#1 RSA keys in PEM format**.

### PEM Format Requirement

> [!IMPORTANT]
> Modern OpenSSH versions default to the proprietary `OPENSSH` key format (`-----BEGIN OPENSSH PRIVATE KEY-----`). The embedded crypto engine requires **PEM format** (`-----BEGIN RSA PRIVATE KEY-----`). Always generate or convert keys using the `-m PEM` parameter.

### Generating & Converting Keys

1. **Generate a new compatible key on your workstation or server**:
   ```bash
   ssh-keygen -t rsa -b 4096 -m PEM -f ~/.ssh/id_rsa -N ""
   cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
   chmod 600 ~/.ssh/authorized_keys ~/.ssh/id_rsa
   ```

2. **Convert an existing OpenSSH key to PEM format**:
   ```bash
   ssh-keygen -p -m PEM -f ~/.ssh/id_rsa
   ```

3. **Verify the key header**:
   ```bash
   head -n 2 ~/.ssh/id_rsa
   # Expected output:
   # -----BEGIN RSA PRIVATE KEY-----
   ```

### Remote Key Fetching (`getkey` / `:getkey`)

You do not need to take out the MicroSD card to install new keys. You can fetch keys over an active SSH connection:

1. Connect to your server with password authentication:
   ```text
   ssh 192.168.1.100 22 pi raspberry
   ```
2. Switch to Local Shell with **Alt + Enter** (or use the `:getkey` escape command).
3. Type:
   ```text
   getkey
   ```
4. PocketSSH scans the remote `~/.ssh/` folder and opens an interactive touch modal dialog showing available `.pem` files.
5. Tap the desired key. PocketSSH downloads it, validates the PEM header, writes it to `/sdcard/ssh_keys/<name>.pem`, and mirrors it into ESP32 NVS flash.
6. Now you can authenticate using public key auth:
   ```text
   sshkey 192.168.1.100 22 pi id_rsa.pem
   ```

### MicroSD Directory Structure

```text
/sdcard/
├── wifi.cfg                 ← Plaintext WiFi credentials (SSID\nPASSWORD)
├── wificonfig.json          ← Structured WiFi configuration backup
└── ssh_keys/
    ├── id_rsa.pem           ← Primary server key
    ├── rpi_server.pem       ← Raspberry Pi lab key
    └── production.pem       ← Cloud VPS key
```

---

## Verification & Validation (V&V) Guide

To prevent regressions, PocketSSH features a multi-tiered Verification & Validation system consisting of host pre-build unit tests and full virtual target integration tests in QEMU:

### Host Pre-Build Unit & Integration Tests

Located in [tests/test_terminal.py](tests/test_terminal.py), this Python test suite exercises 28 unit and integration tests across 6 core subsystems in under 5 milliseconds:

```powershell
pwsh -File .\tests\run_tests.ps1
```

```text
==========================================================
  POCKETSSH PRE-BUILD VERIFICATION AND VALIDATION TESTS
==========================================================
test_ansi_16_standard_colors ... ok
test_ansi_256_cube_colors ... ok
test_ansi_256_grayscale ... ok
test_truecolor_formatting ... ok
test_end_of_line_detection ... ok
test_non_destructive_cursor_clamping ... ok
test_start_of_line_detection ... ok
test_alt_enter_toggles_local_shell ... ok
test_backspace_handled_in_alt_mode ... ok
test_cd_tilde_slash_passed_to_remote_shell ... ok
test_empty_enter_produces_prompt_in_local_shell ... ok
test_exit_when_disconnected_does_not_claim_session_closed ... ok
test_getkey_intercepted_locally ... ok
test_getkey_without_args_intercepted_locally ... ok
test_keys_intercepted_locally ... ok
test_regular_command_passed_to_remote_shell ... ok
test_test_command_passed_to_remote_shell ... ok
test_tilde_slash_never_intercepted_as_escape ... ok
test_vim_commands_passed_to_remote_shell ... ok
test_key_name_resolution_fallback ... ok
test_nvs_keyname_clamping ... ok
test_openssh_format_rejected_with_warning ... ok
test_pem_format_accepted ... ok
test_backspace_blocked_at_start_of_line ... ok
test_carriage_return_does_not_modify_text ... ok
test_all_missing_symbols ... ok
test_alt_space_tab_injection ... ok
test_simulated_ssh_session_with_getkey ... ok

----------------------------------------------------------------------
Ran 28 tests in 0.001s

[OK] ALL 28 SUBSYSTEM UNIT AND INTEGRATION TESTS PASSED (100% VERIFIED)
```

### Virtual ESP32-S3 Target (QEMU) Integration Tests

To test the actual compiled C++ Xtensa firmware binary before flashing physical hardware, PocketSSH supports Espressif's official QEMU emulator (`qemu-system-xtensa`). The firmware automatically detects when running in a virtual target (lacking Octal PSRAM and physical ST7789 SPI display) and boots a responsive **Headless Serial Terminal** over UART0.

#### Running Automated QEMU Tests

Run the automated virtual target test runner script:

```powershell
pwsh -File .\tests\run_qemu_test.ps1
```

```text
==========================================================
  POCKETSSH VIRTUAL TARGET (QEMU) INTEGRATION TESTS
==========================================================
=================================================================
  PocketSSH Virtual ESP32-S3 Target (QEMU) Integration Test
=================================================================
[*] Found QEMU emulator: ...\tools\qemu-xtensa\...\qemu-system-xtensa.exe
[*] Launching PocketSSH in virtual ESP32-S3 target...
[TEST 1/8] Verifying ROM bootloader & virtual target detection... [PASS]
[TEST 2/8] Testing interactive 'version' command... [PASS]
[TEST 3/8] Testing interactive 'help' command... [PASS]
[TEST 4/8] Testing SSH key manager command 'keys'... [PASS]
[TEST 5/8] Testing virtual Wi-Fi scan ('scan')... [PASS]
[TEST 6/8] Testing virtual Wi-Fi connection ('connect 1')... [PASS]
[TEST 7/8] Testing network status report ('status')... [PASS]
[TEST 8/8] Testing unknown command rejection & prompt recovery... [PASS]
[*] Terminating virtual QEMU target...
=================================================================
[OK] ALL 8/8 VIRTUAL TARGET QEMU TESTS PASSED!
=================================================================
```

#### Running QEMU Interactively

You can launch PocketSSH interactively inside your desktop terminal using the helper script:

```powershell
pwsh -File .\run_qemu.ps1
```

Or run the QEMU binary directly from the project directory (`e:\proj\AiAssited\PocketSSH`):

```powershell
& "E:\proj\sdk\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe" `
    -machine esp32s3 `
    -drive file=E:\proj\AiAssited\PocketSSH\build\qemu_flash.bin,if=mtd,format=raw `
    -drive file=E:\proj\AiAssited\PocketSSH\build\qemu_efuse.bin,if=none,format=raw,id=efuse `
    -nographic -serial mon:stdio
```

You can type local commands like `help`, `version`, `keys`, `scan`, `connect 1`, `status`, etc., directly into your PowerShell terminal with live character echo and history editing. Press `Ctrl+A` then `X` to exit QEMU.

---

### Wokwi Simulation (Full GUI, Wi-Fi & SD Card Simulation)

PocketSSH includes out-of-the-box configuration for [Wokwi](https://wokwi.com), enabling complete hardware simulation including the **ST7789 320x240 LCD display**, **MicroSD Card**, **Octal PSRAM (8MB)**, and **real Wi-Fi internet access (`Wokwi-GUEST`)**.

#### 1. Interactive Visual Simulation in VS Code
1. Install the official **Wokwi Simulator** extension from the VS Code Marketplace.
2. Build the firmware: `.\build_firmware.ps1`.
3. Open `diagram.json` in VS Code and press **F1** -> **Wokwi: Start Simulator**.
4. The virtual LilyGO T-Deck will boot with live graphics rendering on the 320x240 screen, accepting keyboard input and connecting to virtual Wi-Fi.

#### 2. Running Automated Wokwi Tests
Automated headless scenario testing is handled via `wokwi-cli`:

```powershell
# Run the automated scenario test runner:
pwsh -File .\tests\run_wokwi_test.ps1
```

*Note: Automated CLI simulation requires a free Wokwi CI token from [wokwi.com/dashboard/ci](https://wokwi.com/dashboard/ci). Set it in PowerShell: `$env:WOKWI_CLI_TOKEN = "wok_..."`.*

#### 3. Continuous Integration (GitHub Actions)
PocketSSH includes `.github/workflows/wokwi_test.yml` which automatically compiles firmware with ESP-IDF and executes the `tests/wokwi_scenario.yaml` integration suite on every push and pull request.

### Automated Testing Strategy
Testing is decoupled from physical hardware through a tiered multi-level architecture:
- **Host Unit & Integration Tests**: Run in milliseconds via `tests/run_tests.ps1` before every build, ensuring zero regression in terminal emulation, character mapping, and session routing.
- **Virtual ESP32-S3 QEMU Tests**: Run in ~8 seconds via `tests/run_qemu_test.ps1` after every build, booting the real ESP32-S3 Xtensa binary and validating FreeRTOS kernel startup, heap allocations, command dispatch, and serial line discipline 100% offline.
- **Wokwi Simulator CI**: Validates the visual ST7789 display controller, Octal PSRAM initialization, real `Wokwi-GUEST` network handshake, and SD card file systems.

---

## Architecture & Technical Details

```mermaid
flowchart TD
    subgraph InputSubsystems [Hardware Input Subsystems]
        KB["C3 Keyboard (I2C 0x55)"] --> KeypadTask["keypad_task"]
        TB["Trackball (GPIO 1, 2, 3, 15)"] --> TrackballTask["trackball_task"]
        TP["GT911 Touch Screen"] --> LVGLTask["LVGL GUI Task"]
    end

    subgraph CoreEngine [PocketSSH Terminal Engine]
        KeypadTask --> KeyDispatcher["Key / Alt Mode Dispatcher"]
        TrackballTask --> KeyDispatcher
        KeyDispatcher --> ModeRouter{"Mode Router"}
        ModeRouter -->|Interactive SSH| StreamWriter["libssh2 Channel Writer"]
        ModeRouter -->|Local Shell| LocalCmdProc["Local Command Processor"]
        LocalCmdProc --> KeyMgr["Key Manager / NVS"]
        LocalCmdProc --> WiFiMgr["WiFi Manager"]
    end

    subgraph OutputSubsystems [Display & Storage Engine]
        StreamWriter --> SSHRecv["ssh_receive_task"]
        SSHRecv --> Batcher["256-Byte Stream Batcher"]
        Batcher --> AnsiEngine["ANSI 256 / TrueColor / Emoji Engine"]
        AnsiEngine --> DisplayLock["Thread-Safe Display Lock"]
        DisplayLock --> ST7789["ST7789 LCD (SPI2_HOST)"]
        KeyMgr --> SDCard["MicroSD Card (SPI2_HOST)"]
    end
```

### Shared SPI Bus Architecture
The LilyGO T-Deck Plus shares `SPI2_HOST` between the ST7789 display and the MicroSD card:
- **Display**: Chip Select on `GPIO 12`
- **MicroSD**: Chip Select on `GPIO 39`, MISO on `GPIO 38`
- PocketSSH isolates bus transactions with FreeRTOS mutexes and clean chip-select handling, ensuring the MicroSD card remains permanently mounted at `/sdcard` without flickering or display artifacts.

### FreeRTOS Tasks
- **`keypad_task`**: High-priority I2C polling task for responsive keyboard input.
- **`trackball_task`**: Dedicated GPIO interrupt handler managing history scrolling, arrow generation, and long-press `Ctrl+C` detection.
- **`ssh_receive_task`**: Handles non-blocking socket reads (`4 KB` buffer) and passes incoming data to the chunk batcher.
- **`LVGL task`**: Renders terminal display updates, status bars, and floating HUD widgets.

### Display Performance & Batching
- **256-Byte Batching**: Flushes output stream data in 256-byte bursts, eliminating display lock starvation.
- **Zero-Timeout Display Locks**: `bsp_display_lock(0)` ensures UI rendering takes precedence, dropping non-critical frames gracefully under heavy network bursts.

---

## Building & Flashing

### Pre-Built Merged Binary (Offset 0x0)

The release image is generated as a single pre-merged binary combining the bootloader, partition table, and application firmware:

- **Binary**: `build/PocketSSH_merged_0x0.bin`
- **Flash Offset**: `0x0`
- **Flash Mode**: `dio`, `80MHz`, `16MB`

#### Flash with ESP Web Flasher (Browser)
1. Open [ESP Web Flasher](https://espressif.github.io/esptool-js/) in Chrome/Edge.
2. Connect your T-Deck via USB-C and select the serial port.
3. Choose `build/PocketSSH_merged_0x0.bin` at offset **`0x0`**.
4. Click **Program**.

#### Flash with esptool (Command Line)
```bash
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 build/PocketSSH_merged_0x0.bin
```

### Building from Source

#### Prerequisites
- [ESP-IDF v5.4 or v5.5](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- Python 3.10+
- PowerShell (Windows) or bash (macOS/Linux)

#### Automated Build & Test Script
Run [build_firmware.ps1](build_firmware.ps1) from the repository root:
```powershell
.\build_firmware.ps1
```
This script:
1. Executes the 28-test host V&V test suite.
2. Halts compilation immediately if any pre-build test fails.
3. Compiles the project using `idf.py build`.
4. Merges all components into `build/PocketSSH_merged_0x0.bin` (compact for flashing to hardware).
5. Generates `build/qemu_flash.bin` (16MB padded MTD image for virtual execution).
6. Executes the 5-test QEMU Virtual ESP32-S3 Target integration suite.

To flash directly using the build script:
```powershell
.\flash.ps1 -Port COM5
```

---

## Version History

### v1.3.0 (October 31 - November 1, 2026)
- **Added**: Virtual ESP32-S3 Target (QEMU) integration test harness (`tests/run_qemu_test.ps1`, `tests/test_qemu.py`) and Headless Serial Terminal over UART0 with automatic target detection.
- **Added**: Alt+Enter Local Shell Toggle (`[local]> `) allowing seamless switching between remote SSH and local commands without disconnecting.
- **Added**: Interactive Remote Key Fetcher (`getkey` / `:getkey`) with touch file picker modal and PEM format checking.
- **Added**: Pre-Build Verification & Validation (V&V) Host Test Suite (28 automated unit & integration tests) integrated into `build_firmware.ps1`.
- **Added**: Pure Tilde (`~/`) passthrough deconfliction (zero delay, no escape buffering or double-echoing).
- **Added**: Single pre-merged binary release (`PocketSSH_merged_0x0.bin` at offset `0x0`) and 16MB QEMU image (`qemu_flash.bin`).
- **Added**: MicroSD Card Permanent Mount & Shared SPI Bus Architecture on `SPI2_HOST`.
- **Added**: Real-Time SSH Activity Indicator (`[WAIT...]`, live rotating spinner, `[ONLINE]`).
- **Added**: Dynamic Alt Mode, Dual-Behavior Trackball (Scroll vs Arrows), and 16 missing symbol chords.
- **Added**: Terminal Line Discipline: non-destructive backspace clamping and separate `\r` vs `\n` handling.
- **Added**: Full ANSI SGR 256-Color & TrueColor Engine with literal `#` syntax isolation.
- **Added**: Smart Emoji Log Engine & Multi-Byte UTF-8 Stream Parser (65+ developer emoji badges).
- **Added**: 2.4 GHz WiFi Scanner (`scan`), WPA3-SAE Personal authentication, and persistent auto-connect.
- **Added**: 16 KB Circular Scrollback Buffer with momentum touch scrolling and trackball navigation.

### v1.2.0 (February 1, 2026)
- **Added**: SSH public key authentication support via MicroSD card.
- **Added**: Quote-aware argument parsing for WiFi networks with spaces.
- **Improved**: `libssh2_userauth_publickey_frommemory()` integration.

### v1.1.0 (January 14, 2026)
- **Fixed**: Boot loop issue on rapid power cycles (GPIO 4 ADC settling delay).
- **Added**: Touch-to-position cursor functionality.

### v1.0.0 (Initial Release)
- Full SSH2 terminal with PTY support.
- Hardware keyboard, trackball navigation, and touch gestures.
- Battery monitoring and NVS command history.

---

## Troubleshooting

### WiFi Connection Issues
- **`[201] AP not found`**: Ensure you are connecting to a 2.4 GHz network. ESP32-S3 does not support 5 GHz bands.
- **`[202] Authentication failed`**: Verify your password. If your SSID or password contains spaces, enclose them in double quotes (e.g. `connect "My Network" "My Password"`).
- **Mesh / Dual-Band Routers**: If your router shares one SSID for 2.4 GHz and 5 GHz, use `scan` to find the exact scan index of the 2.4 GHz BSSID and connect via `connect <#> <password>`.

### SSH Connection Issues
- **`command not found: :getkey`**: You typed `:getkey` in the remote shell instead of the local prompt. Press **Alt + Enter** to enter Local Shell mode (`[local]> `), then type `getkey`.
- **`OpenSSH format rejected`**: Modern OpenSSH keys are generated in `OPENSSH` format. Run `ssh-keygen -p -m PEM -f ~/.ssh/id_rsa` on your server to convert to PKCS#1 PEM format.
- **`Key file not found`**: Ensure the `.pem` file exists in `/sdcard/ssh_keys/` or fetch it directly using `getkey`. Type `keys` to view loaded keys.

### Keyboard & Trackball
- **Typing missing symbols**: Click the trackball to toggle Alt Mode (yellow HUD banner appears), then press the shortcut key (e.g., `t` for `~`, `p` for `|`, `Space` for `Tab`).
- **Exiting runaway programs**: Long-press the trackball for >400 ms or press physical **`Alt + C`** to send `Ctrl+C` (`\x03`).

---

## License & Credits

- **ESP-IDF**: Apache License 2.0 (Espressif Systems)
- **LVGL**: MIT License
- **libssh2**: BSD-3-Clause License
- **PocketSSH**: MIT License
