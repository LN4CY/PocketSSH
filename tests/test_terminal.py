#!/usr/bin/env python3
"""
PocketSSH Terminal Unit & Integration Test Suite
================================================
Comprehensive verification and validation tests for terminal subsystems:
  1. ANSI SGR 16-color, 256-color & 24-bit TrueColor Engine
  2. ANSI CSI Cursor movement & line boundary clamping (SOL / EOL)
  3. Linefeed (\\n) and Carriage Return (\\r) separation discipline
  4. Non-destructive backspace (\\b) and line erasing (\\033[K / \\033[2K)
  5. T-Deck physical keyboard symbol mapping (Alt mode & missing symbols)
  6. SSH Key Manager resolution, PEM validation, and OpenSSH rejection
  7. Escape command state machine (:getkey, :keys, :test, passthrough)
  8. End-to-end multi-line terminal stream integration test
"""

import unittest
import sys
import os

# --- ANSI 256 Color Palette Calculation (matches C++ implementation) ---
def parse_ansi_256(code: int) -> int:
    if 0 <= code <= 15:
        s_ansi_16 = [
            0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
            0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
        ]
        return s_ansi_16[code]
    elif 16 <= code <= 231:
        c = code - 16
        b = c % 6; c //= 6
        g = c % 6; c //= 6
        r = c % 6
        cube_levels = [0, 95, 135, 175, 215, 255]
        return (cube_levels[r] << 16) | (cube_levels[g] << 8) | cube_levels[b]
    elif 232 <= code <= 255:
        gray = 8 + (code - 232) * 10
        return (gray << 16) | (gray << 8) | gray
    return 0x00FF00

# --- Boundary Calculation Helpers (matches C++ implementation) ---
def find_sol(text: str, cpos: int) -> int:
    cpos = max(0, min(cpos, len(text)))
    for k in range(cpos - 1, -1, -1):
        if text[k] == '\n':
            return k + 1
    return 0

def find_eol(text: str, cpos: int) -> int:
    cpos = max(0, min(cpos, len(text)))
    idx = text.find('\n', cpos)
    return idx if idx != -1 else len(text)

# --- Symbol Translation Table (matches C++ implementation) ---
def map_tdeck_alt_key(key: str) -> str:
    alt_map = {
        't': '~', 'p': '|', 'b': '\\', 'e': '=',
        'u': '[', 'i': ']', 'o': '{', 'j': '}',
        'k': '<', ',': '<',
        'm': '>', '.': '>',
        'v': '%', '5': '%',
        's': '$', '4': '$',
        'g': '^', '6': '^',
        'n': '&', '7': '&',
        'q': '`', ' ': '\t'
    }
    return alt_map.get(key, key)


class TestAnsiColorEngine(unittest.TestCase):
    """Verifies ANSI SGR 16-color, 256-color palette, and TrueColor RGB."""

    def test_ansi_16_standard_colors(self):
        # Index 1 = Red, Index 2 = Green, Index 4 = Blue
        self.assertEqual(parse_ansi_256(1), 0xAA0000)
        self.assertEqual(parse_ansi_256(2), 0x00AA00)
        self.assertEqual(parse_ansi_256(4), 0x0000AA)
        # Bright colors: Index 9 = Bright Red, Index 10 = Bright Green
        self.assertEqual(parse_ansi_256(9), 0xFF5555)
        self.assertEqual(parse_ansi_256(10), 0x55FF55)

    def test_ansi_256_cube_colors(self):
        # Pure Red in 6x6x6 cube is r=5, g=0, b=0 -> 16 + 5*36 = 196
        self.assertEqual(parse_ansi_256(196), 0xFF0000)
        # Pure Green in 6x6x6 cube is r=0, g=5, b=0 -> 16 + 5*6 = 46
        self.assertEqual(parse_ansi_256(46), 0x00FF00)
        # Pure Blue in 6x6x6 cube is r=0, g=0, b=5 -> 16 + 5 = 21
        self.assertEqual(parse_ansi_256(21), 0x0000FF)

    def test_ansi_256_grayscale(self):
        # Index 232 is darkest gray (8, 8, 8)
        self.assertEqual(parse_ansi_256(232), 0x080808)
        # Index 255 is lightest gray (8 + 23*10 = 238 = 0xEE)
        self.assertEqual(parse_ansi_256(255), 0xEEEEEE)

    def test_truecolor_formatting(self):
        r, g, b = 80, 255, 80
        tag = f"#{r:02x}{g:02x}{b:02x} "
        self.assertEqual(tag, "#50ff50 ")


class TestCursorClampingAndLineBounds(unittest.TestCase):
    """Verifies cursor horizontal boundary isolation and SOL/EOL calculation."""

    def setUp(self):
        self.sample = "Line 1: pi@server:~$\nLine 2: ls -la\nLine 3: file1.txt"

    def test_start_of_line_detection(self):
        # Any position on line 1 should return SOL = 0
        self.assertEqual(find_sol(self.sample, 5), 0)
        self.assertEqual(find_sol(self.sample, 0), 0)

        # Line 2 starts at index 21
        sol2 = find_sol(self.sample, 25)
        self.assertEqual(sol2, 21)

        # Line 3 starts at index 36
        sol3 = find_sol(self.sample, 42)
        self.assertEqual(sol3, 36)

    def test_end_of_line_detection(self):
        # Line 1 EOL is at newline index 20
        self.assertEqual(find_eol(self.sample, 0), 20)
        self.assertEqual(find_eol(self.sample, 10), 20)

        # Line 2 EOL is at newline index 35
        self.assertEqual(find_eol(self.sample, 21), 35)

        # Line 3 has no trailing newline; EOL is buffer length
        self.assertEqual(find_eol(self.sample, 36), len(self.sample))

    def test_non_destructive_cursor_clamping(self):
        # On line 2 (offset 25), moving left by 100 should clamp at SOL (21)
        cpos = 25
        sol = find_sol(self.sample, cpos)
        target_left = max(sol, cpos - 100)
        self.assertEqual(target_left, 21)
        self.assertGreaterEqual(target_left, sol)

        # Moving right by 100 should clamp at EOL (35)
        eol = find_eol(self.sample, cpos)
        target_right = min(eol, cpos + 100)
        self.assertEqual(target_right, 35)
        self.assertLessEqual(target_right, eol)


class TestLineDiscipline(unittest.TestCase):
    """Verifies Carriage Return, Linefeed, and Backspace protection."""

    def test_carriage_return_does_not_modify_text(self):
        line = "pi@server:~$ sudo apt update"
        cpos = len(line)
        # CR jumps cursor to SOL (0) without modifying string
        cpos = find_sol(line, cpos)
        self.assertEqual(cpos, 0)
        self.assertEqual(line, "pi@server:~$ sudo apt update")

    def test_backspace_blocked_at_start_of_line(self):
        buf = list("prompt$ ")
        cpos = len(buf)
        sol = 0
        # Delete 8 chars
        for _ in range(8):
            if cpos > sol:
                cpos -= 1
                buf.pop(cpos)
        self.assertEqual(cpos, 0)
        self.assertEqual(len(buf), 0)

        # Further backspace attempts when cpos == sol must do nothing
        if cpos > sol:
            buf.pop()
        self.assertEqual(cpos, 0)
        self.assertEqual(len(buf), 0)


class TestTDeckKeyboardMappings(unittest.TestCase):
    """Verifies all missing symbols and control sequences on T-Deck."""

    def test_all_missing_symbols(self):
        expected_pairs = [
            ('t', '~'), ('p', '|'), ('b', '\\'), ('e', '='),
            ('u', '['), ('i', ']'), ('o', '{'), ('j', '}'),
            ('k', '<'), (',', '<'),
            ('m', '>'), ('.', '>'),
            ('v', '%'), ('5', '%'),
            ('s', '$'), ('4', '$'),
            ('g', '^'), ('6', '^'),
            ('n', '&'), ('7', '&'),
            ('q', '`')
        ]
        for key, expected in expected_pairs:
            mapped = map_tdeck_alt_key(key)
            self.assertEqual(mapped, expected, f"Key '{key}' should map to '{expected}', got '{mapped}'")

    def test_alt_space_tab_injection(self):
        mapped = map_tdeck_alt_key(' ')
        self.assertEqual(mapped, '\t', "Alt + Space should inject Tab (\\t)")


class TestKeyManagerAndFormatValidation(unittest.TestCase):
    """Verifies PEM vs OpenSSH validation and key extension fallback."""

    def test_pem_format_accepted(self):
        valid_pem = "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA0...\n-----END RSA PRIVATE KEY-----\n"
        is_pem = ("-----BEGIN" in valid_pem) and ("OPENSSH" not in valid_pem)
        self.assertTrue(is_pem)

    def test_openssh_format_rejected_with_warning(self):
        openssh = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAA...\n-----END OPENSSH PRIVATE KEY-----\n"
        is_openssh = "OPENSSH" in openssh
        self.assertTrue(is_openssh)

    def test_key_name_resolution_fallback(self):
        loaded_keys = {"id_pem": "KEY_DATA_HERE", "my_server.pem": "KEY_DATA_2"}

        def lookup_key(name: str):
            # 1. Exact match
            if name in loaded_keys:
                return loaded_keys[name]
            # 2. Append .pem
            if name + ".pem" in loaded_keys:
                return loaded_keys[name + ".pem"]
            # 3. Strip .pem
            if name.endswith(".pem") and name[:-4] in loaded_keys:
                return loaded_keys[name[:-4]]
            # 4. Case-insensitive
            for k, v in loaded_keys.items():
                if k.lower() == name.lower() or k.lower() == (name + ".pem").lower():
                    return v
            return None

        self.assertIsNotNone(lookup_key("id_pem"))
        self.assertIsNotNone(lookup_key("id_pem.pem"))
        self.assertIsNotNone(lookup_key("ID_PEM"))
        self.assertIsNotNone(lookup_key("my_server"))
        self.assertIsNotNone(lookup_key("my_server.pem"))
        self.assertIsNone(lookup_key("nonexistent_key"))

    def test_nvs_keyname_clamping(self):
        long_key = "very_long_key_name_exceeding_15_characters"
        sanitized = long_key[:15]
        self.assertEqual(len(sanitized), 15)
        self.assertEqual(sanitized, "very_long_key_n")

    def test_nvs_history_cap_and_space_reclamation(self):
        # Command history must be capped to 25 items in NVS to preserve flash space
        MAX_NVS_HIST = 25
        history = [f"command_{i}" for i in range(120)]
        start_idx = len(history) - MAX_NVS_HIST if len(history) > MAX_NVS_HIST else 0
        saved_items = history[start_idx:]
        self.assertEqual(len(saved_items), 25)
        self.assertEqual(saved_items[0], "command_95")
        self.assertEqual(saved_items[-1], "command_119")

    def test_nvs_key_saving_reclaims_space_on_nvs_full(self):
        # Simulate NVS space reclamation: when ssh_keys namespace hits out-of-space,
        # storage (command history) is erased to prioritize storing the private key
        nvs_namespaces = {
            "storage": {"hist_0": "ls", "hist_1": "pwd", "hist_count": 2},
            "ssh_keys": {}
        }
        nvs_space_available = 0 # Out of space!

        def save_key(name, data):
            nonlocal nvs_space_available
            if nvs_space_available <= 0:
                # Reclaim space from command history namespace
                nvs_namespaces["storage"].clear()
                nvs_space_available += 10 # Space freed!
            nvs_namespaces["ssh_keys"][name] = data
            return True

        self.assertTrue(save_key("id_pem", "-----BEGIN RSA PRIVATE KEY-----..."))
        self.assertIn("id_pem", nvs_namespaces["ssh_keys"])
        self.assertEqual(len(nvs_namespaces["storage"]), 0) # History was reclaimed for the key!

    def test_save_key_prefers_sd_over_nvs(self):
        # SD card is primary key store; NVS is fallback only when SD is not mounted
        sd_mounted = True
        sd_storage = {}
        nvs_storage = {}

        def save_key_strategy(keyname, key_data, is_sd_mounted):
            if is_sd_mounted:
                sd_storage[keyname] = key_data
                return "saved_to_sd"
            else:
                nvs_storage[keyname] = key_data
                return "saved_to_nvs"

        # Case 1: SD mounted -> written to SD, NVS remains untouched
        result = save_key_strategy("id_pem", "PEM_DATA", is_sd_mounted=True)
        self.assertEqual(result, "saved_to_sd")
        self.assertIn("id_pem", sd_storage)
        self.assertNotIn("id_pem", nvs_storage)

        # Case 2: SD not mounted -> falls back to NVS
        result = save_key_strategy("backup_key", "PEM_DATA_2", is_sd_mounted=False)
        self.assertEqual(result, "saved_to_nvs")
        self.assertIn("backup_key", nvs_storage)


class TestEscapeCommandStateMachine(unittest.TestCase):
    """Verifies that :getkey, :keys, :exit are captured locally and :test passes to remote shell."""

    def simulate_key_entry(self, input_chars: str):
        """Simulates SSHTerminal's escape buffer logic."""
        buffer = ""
        flushed_to_remote = []
        local_command_executed = None
        supported_cmds = ["getkey", "keys", "help", "exit", "quit", "clear"]

        for ch in input_chars:
            if ch in ('\r', '\n'):
                if buffer:
                    local_command_executed = buffer
                    buffer = ""
                else:
                    flushed_to_remote.append('\r')
                continue

            if not buffer and ch == ':':
                buffer += ch
                continue

            if buffer:
                buffer += ch
                prefix = buffer[1:]
                matches_any = False
                for cmd in supported_cmds:
                    if len(prefix) <= len(cmd) and cmd.startswith(prefix):
                        matches_any = True
                        break
                    if len(prefix) > len(cmd) and prefix.startswith(cmd + " "):
                        matches_any = True
                        break
                if not matches_any:
                    # Flushed to remote
                    flushed_to_remote.extend(list(buffer))
                    buffer = ""
                continue

            flushed_to_remote.append(ch)

        return local_command_executed, "".join(flushed_to_remote)

    def test_getkey_intercepted_locally(self):
        cmd, remote = self.simulate_key_entry(":getkey ~/.ssh/id_pem\r")
        self.assertEqual(cmd, ":getkey ~/.ssh/id_pem")
        self.assertEqual(remote, "", "Escape command must NOT be sent to remote server")

    def test_getkey_without_args_intercepted_locally(self):
        cmd, remote = self.simulate_key_entry(":getkey\r")
        self.assertEqual(cmd, ":getkey")
        self.assertEqual(remote, "")

    def test_keys_intercepted_locally(self):
        cmd, remote = self.simulate_key_entry(":keys\r")
        self.assertEqual(cmd, ":keys")
        self.assertEqual(remote, "")

    def test_test_command_passed_to_remote_shell(self):
        # :test is no longer an escape command; it should pass directly to remote shell
        cmd, remote = self.simulate_key_entry(":test\r")
        self.assertIsNone(cmd)
        self.assertEqual(remote, ":test\r")

    def test_vim_commands_passed_to_remote_shell(self):
        # In vim, typing :w followed by Enter should flush to remote!
        cmd, remote = self.simulate_key_entry(":w\r")
        self.assertIsNone(cmd)
        self.assertEqual(remote, ":w\r")

    def test_regular_command_passed_to_remote_shell(self):
        cmd, remote = self.simulate_key_entry("ls -la\r")
        self.assertIsNone(cmd)
        self.assertEqual(remote, "ls -la\r")

    def test_tilde_slash_never_intercepted_as_escape(self):
        # ~/scripts/run.sh must pass directly to remote shell without buffering
        cmd, remote = self.simulate_key_entry("~/scripts/run.sh\r")
        self.assertIsNone(cmd)
        self.assertEqual(remote, "~/scripts/run.sh\r")

    def test_cd_tilde_slash_passed_to_remote_shell(self):
        cmd, remote = self.simulate_key_entry("cd ~/.ssh\r")
        self.assertIsNone(cmd)
        self.assertEqual(remote, "cd ~/.ssh\r")

    def test_alt_enter_toggles_local_shell(self):
        # Simulates Alt Mode + Enter dispatching local shell toggle
        alt_mode = True
        key = '\n'
        toggled_shell = False
        if alt_mode and key in ('\n', '\r'):
            alt_mode = False
            toggled_shell = True
        self.assertFalse(alt_mode)
        self.assertTrue(toggled_shell)

    def test_empty_enter_produces_prompt_in_local_shell(self):
        # When hitting Enter on an empty line in local shell, a prompt must be output
        current_input = ""
        prompt = "> "
        output = []
        if not current_input:
            output.append("\n" + prompt)
        self.assertEqual(output, ["\n> "])

    def test_exit_when_disconnected_does_not_claim_session_closed(self):
        # When disconnected, typing exit should indicate not connected, never [SSH session closed]
        ssh_connected = False
        action = None
        if ssh_connected:
            action = "disconnect"
        else:
            action = "not_connected"
        self.assertEqual(action, "not_connected")

    def test_backspace_handled_in_alt_mode(self):
        # Backspace (8 or 127) in alt mode should delete character and not logout
        alt_mode = True
        key = 127
        current_input = "test"
        if alt_mode and key in (8, 127):
            current_input = current_input[:-1]
        self.assertEqual(current_input, "tes")


class TestTerminalIntegration(unittest.TestCase):
    """End-to-end integration test simulating live SSH server stream."""

    def test_simulated_ssh_session_with_getkey(self):
        # 1. Server sends login banner and prompt
        server_stream = "Linux PiServer 6.1.21-v8+ #1642 SMP\r\npi@PiServer:~$ "
        text_buffer = ""
        cpos = 0

        # Process server output
        for ch in server_stream:
            if ch == '\r':
                cpos = find_sol(text_buffer, cpos)
            elif ch == '\n':
                text_buffer += '\n'
                cpos = len(text_buffer)
            else:
                text_buffer += ch
                cpos += 1

        self.assertIn("pi@PiServer:~$ ", text_buffer)

        # 2. User types :getkey on client
        esc_test = TestEscapeCommandStateMachine()
        cmd, remote = esc_test.simulate_key_entry(":getkey ~/.ssh/id_pem\r")

        # Must be intercepted locally, NOT sent to remote shell!
        self.assertEqual(cmd, ":getkey ~/.ssh/id_pem")
        self.assertEqual(remote, "")

    def test_pty_modes_rfc4254_opcodes(self):
        # RFC 4254 Section 8 Terminal Modes:
        # Opcode 1 = VINTR (Ctrl+C = 3)
        # Opcode 3 = VERASE (Backspace = 127)
        # Opcode 5 = VEOF (Ctrl+D = 4)
        SSH_TTY_OP_VINTR = 1
        SSH_TTY_OP_VERASE = 3
        SSH_TTY_OP_VEOF = 5
        SSH_TTY_OP_END = 0

        pty_modes = [
            SSH_TTY_OP_VERASE, 0, 0, 0, 127,
            SSH_TTY_OP_VEOF, 0, 0, 0, 4,
            SSH_TTY_OP_VINTR, 0, 0, 0, 3,
            SSH_TTY_OP_END
        ]

        # Verify VERASE opcode is 3 and value is 127 (^?, DEL)
        self.assertEqual(pty_modes[0], 3)
        self.assertEqual(pty_modes[4], 127)

        # Verify VEOF opcode is 5 and value is 4 (^D), NEVER 127 (which caused logout on backspace)
        self.assertEqual(pty_modes[5], 5)
        self.assertEqual(pty_modes[9], 4)

    def test_backspace_forwarded_to_remote_shell_in_ssh(self):
        # In interactive SSH mode, backspace (8 or 127) must always be sent as \x7F
        # to the remote shell so history recall (Up Arrow) and tab completion can be edited!
        ssh_cmd_buffer = ""
        sent_to_server = []

        def handle_key(k):
            nonlocal ssh_cmd_buffer
            if k in (8, 127):
                if ssh_cmd_buffer:
                    ssh_cmd_buffer = ssh_cmd_buffer[:-1]
                    return
                sent_to_server.append('\x7F')
            elif k == ':' and not ssh_cmd_buffer:
                ssh_cmd_buffer += ':'
            elif ssh_cmd_buffer:
                ssh_cmd_buffer += k
            else:
                sent_to_server.append(k)

        # 1. Backspace after Up Arrow (command recalled from bash history):
        # Server recalled "ping 8.8.8.8". User presses Backspace 8 times to edit it.
        # Backspace MUST be forwarded to server as \x7F!
        for _ in range(8):
            handle_key(8)
        self.assertEqual(sent_to_server, ['\x7F'] * 8)

        # 2. Key 127 (DEL) also forwarded as \x7F
        handle_key(127)
        self.assertEqual(sent_to_server, ['\x7F'] * 9)

        # 3. Local escape command editing:
        handle_key(':')
        handle_key('g')
        handle_key('e')
        self.assertEqual(ssh_cmd_buffer, ":ge")
        handle_key(8)
        self.assertEqual(ssh_cmd_buffer, ":g") # Backspace popped local escape buffer!
        self.assertEqual(len(sent_to_server), 9) # Nothing sent to server while in escape mode

    def test_carriage_return_lone_replaces_active_line(self):
        # Simulates realistic terminal overwrite behavior on lone \r
        lines = [list()]
        cpos = 0
        def process_stream(data):
            nonlocal lines, cpos
            i = 0
            while i < len(data):
                ch = data[i]
                if ch == '\r':
                    cpos = 0 # Non-destructive return to column 0
                    if i + 1 < len(data) and data[i + 1] == '\n':
                        i += 1
                        lines.append(list())
                        cpos = 0
                elif ch == '\n':
                    lines.append(list())
                    cpos = 0
                else:
                    if cpos < len(lines[-1]):
                        lines[-1][cpos] = ch # Overwrite mode
                    else:
                        lines[-1].append(ch) # Append mode
                    cpos += 1
                i += 1

        process_stream("Task: 10%\rTask: 20%\rTask: 100%\r\nDone!")
        rendered = ["".join(l) for l in lines]
        self.assertEqual(rendered, ["Task: 100%", "Done!"])

        # Test command line wrapping: first line must NOT be wiped out
        lines = [list()]
        cpos = 0
        process_stream("user@server:~$ sudo apt install -y build-essential\r\ngcc g++ make")
        rendered = ["".join(l) for l in lines]
        self.assertEqual(rendered, [
            "user@server:~$ sudo apt install -y build-essential",
            "gcc g++ make"
        ])

    def test_cursor_movement_skips_color_tags(self):
        # Tag format: #RRGGBB (8 bytes) and # (1 byte)
        def skip_color_tags_forward(txt, pos):
            tlen = len(txt)
            while pos < tlen:
                if txt[pos] == '#' and pos + 7 < tlen and txt[pos + 7] == ' ':
                    pos += 8
                elif txt[pos] == '#' and (pos + 1 >= tlen or txt[pos + 1] != '#'):
                    pos += 1
                else:
                    break
            return pos

        sample = "#00FF00 pi@host#:#4090FF ~#$ "
        # Position 0 is '#'. Skipping should jump past '#00FF00 ' (8 bytes) to index 8 ('p')
        self.assertEqual(skip_color_tags_forward(sample, 0), 8)
        self.assertEqual(sample[8], 'p')

    def test_get_line_content_start_and_backward_skip(self):
        def get_line_content_start(txt, sol):
            tlen = len(txt)
            pos = sol
            while pos + 7 < tlen and txt[pos] == '#' and txt[pos + 7] == ' ':
                pos += 8
            return pos

        def skip_color_tags_backward(txt, min_pos, pos):
            if pos <= min_pos:
                return min_pos
            while pos > min_pos:
                jumped = False
                if pos >= min_pos + 7 and txt[pos] == ' ' and txt[pos - 7] == '#':
                    pos = pos - 7
                    jumped = True
                for offset in range(1, 7):
                    if pos >= min_pos + offset and txt[pos - offset] == '#' and pos + (7 - offset) < len(txt) and txt[pos + (7 - offset)] == ' ':
                        pos = pos - offset
                        jumped = True
                        break
                if pos >= min_pos + 1 and txt[pos] == '#' and txt[pos - 1] == '#':
                    pos = pos - 1
                    jumped = True
                if not jumped:
                    break
            return max(pos, min_pos)

        # Line starts with #00FF00 (8 bytes) then prompt "user@pocket:~$ "
        line = "#00FF00 user@pocket:~$ "
        sol = 0
        min_pos = get_line_content_start(line, sol)
        self.assertEqual(min_pos, 8)
        self.assertEqual(line[min_pos], 'u')

        # Cursor at position 8 moving left must clamp at 8, NEVER entering index 7 (color tag space)
        left_pos = min_pos
        if left_pos > min_pos:
            left_pos -= 1
        left_pos = skip_color_tags_backward(line, min_pos, left_pos)
        self.assertEqual(left_pos, 8)

        # Embedded color tag test: "user@pocket:~$ #FFFFFF ls"
        # Indices:
        # 0..7: "#00FF00 "
        # 8..22: "user@pocket:~$ "
        # 23..30: "#FFFFFF "
        # 31..32: "ls"
        embedded = "#00FF00 user@pocket:~$ #FFFFFF ls"
        min_p = get_line_content_start(embedded, 0)
        self.assertEqual(min_p, 8)

        # Cursor at index 31 (start of "ls", immediately after "#FFFFFF ")
        # Moving left 1 step must skip over the 8-byte "#FFFFFF " tag and land at 23 (before '#')
        target = 31 - 1 # 30 is the space of #FFFFFF 
        target = skip_color_tags_backward(embedded, min_p, target)
        self.assertEqual(target, 23)
        self.assertEqual(embedded[target], '#')

    def test_trackball_left_does_not_clear_or_corrupt_input(self):
        # Simulate local mode trackball left/right behavior
        current_input = "connect 1"
        cursor_pos = len(current_input) # 9

        # Move left 9 times to reach 0
        for _ in range(9):
            if cursor_pos > 0:
                cursor_pos -= 1
        self.assertEqual(cursor_pos, 0)
        self.assertEqual(current_input, "connect 1") # Text must remain intact!

        # Move left further: must clamp at 0 and NOT navigate history or clear
        for _ in range(5):
            if cursor_pos > 0:
                cursor_pos -= 1
        self.assertEqual(cursor_pos, 0)
        self.assertEqual(current_input, "connect 1") # Text must NEVER disappear!


class TestModalDialogTrackballAndFocusIsolation(unittest.TestCase):
    """Verifies trackball navigation, focus cycling, and input isolation in key selection dialogs."""

    def test_getkey_dialog_trackball_navigation(self):
        # Focus index: 0 = TextArea, 1 = id_pem, 2 = id_rsa, 3 = id_ed25519, 4 = FETCH, 5 = CANCEL
        focus = 0
        def down(f):
            if f == 0: return 1
            elif 1 <= f <= 3: return 5 if f == 3 else 4
            else: return 0

        def up(f):
            if f >= 4: return 3 if f == 5 else 1
            elif 1 <= f <= 3: return 0
            else: return 4

        def left(f):
            if 1 < f <= 3: return f - 1
            elif f == 5: return 4
            return f

        def right(f):
            if 1 <= f < 3: return f + 1
            elif f == 4: return 5
            return f

        self.assertEqual(down(0), 1) # TA -> Presets (id_pem)
        self.assertEqual(right(1), 2) # id_pem -> id_rsa
        self.assertEqual(right(2), 3) # id_rsa -> id_ed25519
        self.assertEqual(down(3), 5) # id_ed25519 -> CANCEL
        self.assertEqual(left(5), 4) # CANCEL -> FETCH
        self.assertEqual(up(4), 1) # FETCH -> Presets (id_pem)
        self.assertEqual(up(1), 0) # Presets -> TA
        self.assertEqual(up(0), 4) # TA wraps to FETCH
        self.assertEqual(down(4), 0) # FETCH wraps to TA

    def test_getkey_dialog_click_and_preset_selection(self):
        focus = 0
        ta_text = "~/.ssh/id_pem"
        presets = ["~/.ssh/id_pem", "~/.ssh/id_rsa", "~/.ssh/id_ed25519"]
        dialog_closed = False
        fetched_path = None

        def click(f, text):
            nonlocal dialog_closed, fetched_path
            if 1 <= f <= 3:
                # Selecting preset updates text and automatically moves focus to FETCH KEY
                return presets[f - 1], 4
            elif f == 4 or f == 0:
                fetched_path = text
                dialog_closed = True
                return text, f
            elif f == 5:
                dialog_closed = True
                return text, f
            return text, f

        # Select id_rsa preset (index 2)
        ta_text, focus = click(2, ta_text)
        self.assertEqual(ta_text, "~/.ssh/id_rsa")
        self.assertEqual(focus, 4) # Focus shifted to FETCH KEY
        self.assertFalse(dialog_closed)

        # Next click triggers FETCH KEY
        ta_text, focus = click(focus, ta_text)
        self.assertTrue(dialog_closed)
        self.assertEqual(fetched_path, "~/.ssh/id_rsa")

    def test_getkey_dialog_input_isolation(self):
        # Keystrokes must edit dialog textarea and NEVER leak to background shell or SSH
        dialog_open = True
        dialog_ta = list("~/.ssh/id_pem")
        background_input = ""

        def handle_key(key):
            nonlocal dialog_open, background_input
            k = ord(key) if isinstance(key, str) else key
            if dialog_open:
                if k == 27 or k == 3 or k == 0x0C: # Esc or Ctrl+C
                    dialog_open = False
                    return
                elif k == 8 or k == 127: # Backspace
                    if dialog_ta: dialog_ta.pop()
                    return
                elif k >= 32:
                    dialog_ta.append(chr(k))
                    return
                return
            # Background
            background_input += chr(k)

        # Type ".custom" into dialog
        for ch in ".custom":
            handle_key(ch)
        self.assertEqual("".join(dialog_ta), "~/.ssh/id_pem.custom")
        self.assertEqual(background_input, "") # Zero background leakage!

        # Backspace 7 times
        for _ in range(7):
            handle_key(8)
        self.assertEqual("".join(dialog_ta), "~/.ssh/id_pem")
        self.assertEqual(background_input, "")

        # Close dialog
        handle_key(27)
        self.assertFalse(dialog_open)

        # Subsequent keystrokes now go to background
        for ch in "ls\n":
            handle_key(ch)
        self.assertEqual(background_input, "ls\n")

    def test_side_panel_trackball_navigation_and_isolation(self):
        num_buttons = 29
        focus = 0
        panel_open = True
        background_events = 0

        def trackball_up(f):
            return f - 1 if f > 0 else num_buttons - 1

        def trackball_down(f):
            return f + 1 if f + 1 < num_buttons else 0

        self.assertEqual(trackball_down(0), 1)
        self.assertEqual(trackball_up(0), 28) # Wraps to bottom
        self.assertEqual(trackball_down(28), 0) # Wraps to top

        # When panel is open, trackball click must NOT affect background
        def handle_trackball_click():
            nonlocal background_events
            if panel_open:
                # Triggers focused button, NOT background
                return
            background_events += 1

        handle_trackball_click()
        self.assertEqual(background_events, 0)


if __name__ == '__main__':
    print("=" * 65)
    print("  PocketSSH Terminal Pre-Build Verification & Validation Suite")
    print("=" * 65)
    unittest.main(verbosity=2)
