#!/usr/bin/env python3
"""
PocketSSH Virtual Target (QEMU ESP32-S3) Automated Integration Test Suite
Executes the compiled PocketSSH binary inside Espressif's QEMU emulator,
validates hardware stubs, headless serial initialization, and tests command processing.
"""

import os
import sys
import time
import subprocess
import threading
import glob

def find_qemu():
    # 1. Check PATH
    import shutil
    qemu_on_path = shutil.which("qemu-system-xtensa")
    if qemu_on_path:
        return qemu_on_path

    # 2. Check standard IDF_TOOLS_PATH locations
    search_roots = []
    if "IDF_TOOLS_PATH" in os.environ:
        search_roots.append(os.environ["IDF_TOOLS_PATH"])
    search_roots.extend([
        r"E:\proj\sdk\.espressif",
        os.path.expanduser(r"~/.espressif"),
    ])

    for root in search_roots:
        pattern = os.path.join(root, "tools", "qemu-xtensa", "**", "qemu-system-xtensa.exe")
        matches = glob.glob(pattern, recursive=True)
        if matches:
            return matches[0]

    return None

class QEMURunner:
    def __init__(self, qemu_path, flash_bin, efuse_bin=None):
        self.qemu_path = qemu_path
        self.flash_bin = flash_bin
        self.efuse_bin = efuse_bin
        self.proc = None
        self.output_buffer = ""
        self.lock = threading.Lock()
        self.running = False

    def start(self):
        cmd = [
            self.qemu_path,
            "-machine", "esp32s3",
            "-drive", f"file={self.flash_bin},if=mtd,format=raw",
        ]
        if self.efuse_bin and os.path.exists(self.efuse_bin):
            cmd.extend(["-drive", f"file={self.efuse_bin},if=none,format=raw,id=efuse"])
        cmd.extend(["-nographic", "-serial", "mon:stdio"])

        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=0
        )
        self.running = True
        self.reader_thread = threading.Thread(target=self._read_output, daemon=True)
        self.reader_thread.start()

    def _read_output(self):
        while self.running and self.proc and self.proc.poll() is None:
            try:
                char = self.proc.stdout.read(1)
                if not char:
                    break
                with self.lock:
                    self.output_buffer += char
            except Exception:
                break

    def get_output(self):
        with self.lock:
            return self.output_buffer

    def send_command(self, cmd_text):
        if self.proc and self.proc.stdin:
            self.proc.stdin.write(cmd_text + "\n")
            self.proc.stdin.flush()

    def wait_for(self, target_text, timeout=10.0):
        start = time.time()
        while time.time() - start < timeout:
            with self.lock:
                if target_text in self.output_buffer:
                    return True
            time.sleep(0.05)
        return False

    def stop(self):
        self.running = False
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2.0)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass

def run_tests():
    print("=" * 65)
    print("  PocketSSH Virtual ESP32-S3 Target (QEMU) Integration Test")
    print("=" * 65)

    qemu_bin = find_qemu()
    if not qemu_bin:
        print("[FAIL] qemu-system-xtensa executable not found in IDF_TOOLS_PATH or PATH.")
        sys.exit(1)

    print(f"[*] Found QEMU emulator: {qemu_bin}")

    flash_bin = os.path.abspath("build/qemu_flash.bin")
    if not os.path.exists(flash_bin):
        print(f"[FAIL] Flash image {flash_bin} does not exist. Run build_firmware.ps1 first.")
        sys.exit(1)

    efuse_bin = os.path.abspath("build/qemu_efuse.bin")
    runner = QEMURunner(qemu_bin, flash_bin, efuse_bin)

    passed_count = 0
    total_tests = 8

    try:
        print("[*] Launching PocketSSH in virtual ESP32-S3 target...")
        runner.start()

        # TEST 1: Virtual Target Boot & Banner
        print("[TEST 1/8] Verifying ROM bootloader & virtual target detection...", end="", flush=True)
        if runner.wait_for("[PocketSSH] Virtual Target Boot OK!", timeout=12.0) and \
           runner.wait_for("Environment: Virtual ESP32-S3 Target", timeout=8.0) and \
           runner.wait_for("> ", timeout=5.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Boot Output Captured ---")
            print(runner.get_output())
            print("----------------------------")
            sys.exit(1)

        # TEST 2: Command 'version'
        print("[TEST 2/8] Testing interactive 'version' command...", end="", flush=True)
        runner.send_command("version")
        if runner.wait_for("PocketSSH Version", timeout=6.0) and \
           runner.wait_for("ESP32-S3 LilyGO T-Deck", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 3: Command 'help'
        print("[TEST 3/8] Testing interactive 'help' command...", end="", flush=True)
        runner.send_command("help")
        if runner.wait_for("Available commands:", timeout=6.0) and \
           runner.wait_for("scan", timeout=6.0) and \
           runner.wait_for("connect", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 4: Command 'keys'
        print("[TEST 4/8] Testing SSH key manager command 'keys'...", end="", flush=True)
        runner.send_command("keys")
        if runner.wait_for("Loaded SSH Keys", timeout=6.0) and \
           runner.wait_for("id_ed25519_virtual", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 5: Wi-Fi Scanner 'scan'
        print("[TEST 5/8] Testing virtual Wi-Fi scan ('scan')...", end="", flush=True)
        runner.send_command("scan")
        if runner.wait_for("Scanning 2.4 GHz WiFi networks", timeout=6.0) and \
           runner.wait_for("Wokwi-GUEST", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 6: Wi-Fi Connection 'connect'
        print("[TEST 6/8] Testing virtual Wi-Fi connection ('connect 1')...", end="", flush=True)
        runner.send_command("connect 1")
        if runner.wait_for("Connecting to 'Wokwi-GUEST'", timeout=6.0) and \
           runner.wait_for("WiFi Connected! IP Address: 192.168.1.100", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 7: Network Status 'status'
        print("[TEST 7/8] Testing network status report ('status')...", end="", flush=True)
        runner.send_command("status")
        if runner.wait_for("WiFi Status ===", timeout=6.0) and \
           runner.wait_for("State:    Connected", timeout=6.0) and \
           runner.wait_for("IP:       192.168.1.100", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

        # TEST 8: Unknown Command Handling
        print("[TEST 8/8] Testing unknown command rejection & prompt recovery...", end="", flush=True)
        runner.send_command("testinvalidcmd")
        if runner.wait_for("Unknown command", timeout=6.0):
            print(" [PASS]")
            passed_count += 1
        else:
            print(" [FAIL]")
            print("--- Captured Output ---")
            print(runner.get_output()[-500:])
            print("-----------------------")
            sys.exit(1)

    finally:
        print("[*] Terminating virtual QEMU target...")
        runner.stop()

    print("=" * 65)
    if passed_count == total_tests:
        print(f"[OK] ALL {passed_count}/{total_tests} VIRTUAL TARGET QEMU TESTS PASSED!")
        print("=" * 65)
        sys.exit(0)
    else:
        print(f"[ERROR] Only {passed_count}/{total_tests} tests passed.")
        print("=" * 65)
        sys.exit(1)

if __name__ == "__main__":
    run_tests()
