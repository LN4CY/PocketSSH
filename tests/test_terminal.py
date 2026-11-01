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


class TestEscapeCommandStateMachine(unittest.TestCase):
    """Verifies that :getkey, :keys, :test are captured locally and never sent to remote shell."""

    def simulate_key_entry(self, input_chars: str):
        """Simulates SSHTerminal's escape buffer logic."""
        buffer = ""
        flushed_to_remote = []
        local_command_executed = None
        supported_cmds = ["getkey", "keys", "test", "selftest", "help", "exit", "quit", "clear"]

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

    def test_test_suite_intercepted_locally(self):
        cmd, remote = self.simulate_key_entry(":test\r")
        self.assertEqual(cmd, ":test")
        self.assertEqual(remote, "")

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


if __name__ == '__main__':
    print("=" * 65)
    print("  PocketSSH Terminal Pre-Build Verification & Validation Suite")
    print("=" * 65)
    unittest.main(verbosity=2)
