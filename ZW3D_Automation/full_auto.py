#!/usr/bin/env python3
"""
Full Automatic ZW3D Test
- Start ZW3D automatically
- Create new Part via menu/command automation
- Create block
- Generate 3-view drafting
"""

import subprocess
import time
import sys
import os
import requests

try:
    import win32gui
    import win32con
    import win32api
    import keyboard
    WIN32_AVAILABLE = True
except ImportError:
    WIN32_AVAILABLE = False
    print("ERROR: pywin32 not installed. Please install with:")
    print("  pip install pywin32")
    sys.exit(1)

BASE_URL = "http://localhost:8081"
ZW3D_PATH = r"C:\Program Files\ZWSOFT\ZW3D 2026\ZW3D.exe"

def log(msg):
    # Remove emojis for Windows console compatibility
    msg_clean = msg.replace("✅", "OK:").replace("❌", "ERROR:").replace("⚠️", "WARNING:").replace("🎉", "")
    print(f"[AUTO] {msg_clean}")

def is_server_running():
    """Check if HTTP server is responding"""
    try:
        r = requests.get(f"{BASE_URL}/status", timeout=3)
        return r.status_code == 200
    except:
        return False

def wait_for_server(max_wait=60):
    """Wait for HTTP server to become ready"""
    log(f"Waiting for HTTP server (max {max_wait}s)...")
    for i in range(max_wait):
        if is_server_running():
            log("HTTP server is ready!")
            return True
        time.sleep(1)
        if i % 10 == 0:
            log(f"  Still waiting... {i}/{max_wait}s")
    log("Timeout waiting for server")
    return False

def find_zw3d_window():
    """Find ZW3D main window by title"""
    def callback(handle, extra):
        title = win32gui.GetWindowText(handle)
        if "ZW3D" in title and win32gui.IsWindowVisible(handle):
            extra.append(handle)
        return True

    handles = []
    win32gui.EnumWindows(callback, handles)
    return handles[0] if handles else None

def send_command_to_window(hwnd, command):
    """Activate window and send ZW3D command"""
    log(f"Sending command: {command}")

    # Activate the window
    if win32gui.IsIconic(hwnd):
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
    win32gui.SetForegroundWindow(hwnd)
    time.sleep(0.5)

    # Send the command text
    keyboard.write(command)
    time.sleep(0.2)
    keyboard.press_and_release('enter')
    time.sleep(1.5)

    log("Command sent")

def create_new_part(hwnd):
    """Create new Part using ZW3D command"""
    log("Creating new Part...")

    # Use ZW3D command to create new part
    send_command_to_window(hwnd, "~cvxFileNewSingle")
    # Wait for dialog
    time.sleep(2)
    # Confirm - just press enter
    keyboard.press_and_release('enter')
    time.sleep(3)

    log("New Part created")
    return True

def create_block():
    """Create block via HTTP API"""
    log("Creating block 80x60x40...")
    data = {"length": 80.0, "width": 60.0, "height": 40.0}
    try:
        r = requests.post(f"{BASE_URL}/create_block", json=data, timeout=20)
        result = r.json()
        if result.get("status") == "ok":
            log(f"Block created: {result.get('message')}")
            return True
        else:
            log(f"Block failed: {result.get('message')}")
            return False
    except Exception as e:
        log(f"Block request failed: {e}")
        return False

def generate_drafting():
    """Generate 3-view drafting"""
    log("Generating 3-view drafting...")
    try:
        r = requests.post(f"{BASE_URL}/generate_drafting", json={}, timeout=120)
        result = r.json()
        if result.get("status") == "ok":
            log(f"Drafting generated: {result.get('message')}")
            return True
        else:
            log(f"Drafting failed: {result.get('message')}")
            return False
    except Exception as e:
        log(f"Drafting request failed: {e}")
        return False

def main():
    log("===== Full Automatic ZW3D Test =====")
    log("")

    # Step 1: Check if server already running
    if is_server_running():
        log("HTTP server already running")
        server_was_running = True
    else:
        server_was_running = False
        log("HTTP server not running, will start ZW3D")

    # Step 2: Start ZW3D if needed
    if not server_was_running:
        if not os.path.exists(ZW3D_PATH):
            log(f"ZW3D not found at: {ZW3D_PATH}")
            return
        log(f"Starting ZW3D: {ZW3D_PATH}")
        subprocess.Popen([ZW3D_PATH], shell=True)
        log("ZW3D started, waiting for plugin to load...")
        time.sleep(40)  # Wait for ZW3D to fully load

    # Step 3: Wait for server
    if not wait_for_server(max_wait=60):
        log("Cannot connect to HTTP server. Please check:")
        log("1. Is ZW3D_HTTP_Server.dll loaded?")
        log("2. Check ZW3D message window for errors")
        return

    # Step 4: Find ZW3D window
    log("Finding ZW3D window...")
    hwnd = find_zw3d_window()
    if not hwnd:
        log("Cannot find ZW3D window")
        return
    log(f"Found ZW3D window: handle={hex(hwnd)}")

    # Step 5: Create new Part
    if not create_new_part(hwnd):
        log("Failed to create new Part")
        return

    # Wait longer for everything to stabilize and context to activate
    log("Waiting for ZW3D to complete context switching...")
    time.sleep(8)

    # Step 6: Create block via API
    if not create_block():
        log("Failed to create block")
        log("")
        log("TIP: Check ZW3D message window for detailed error information")
        log("The most common issue is 'WRONG_ROOT_ENV' - ZW3D needs more time")
        log("Try increasing the sleep time in the script if it fails repeatedly")
        return

    time.sleep(3)

    # Step 7: Generate drafting
    if not generate_drafting():
        log("Failed to generate drafting")
        return

    # Done
    log("")
    log("=== FULL AUTOMATIC TEST COMPLETE ===")
    log("")
    log("Output files:")
    log("  - Part: C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftPart.Z3PRT")
    log("  - Drawing: C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3")
    log("")
    log("Please open the drawing file in ZW3D to see the 3 views with dimensions!")

if __name__ == "__main__":
    main()
