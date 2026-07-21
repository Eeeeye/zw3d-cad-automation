#!/usr/bin/env python3
"""
Full Automatic ZW3D Test - Simplified version
- Start ZW3D if not running
- Wait for HTTP server
- Directly call API to create block (API auto-creates new part if needed)
- Generate 3-view drafting
"""

import subprocess
import time
import sys
import os
import requests

BASE_URL = "http://localhost:8081"
ZW3D_PATH = r"C:\Program Files\ZWSOFT\ZW3D 2026\ZW3D.exe"

def log(msg):
    msg_clean = msg.replace("✅", "OK:").replace("❌", "ERROR:").replace("⚠️", "WARNING:").replace("🎉", "")
    print(f"[AUTO] {msg_clean}")

def is_server_running():
    try:
        r = requests.get(f"{BASE_URL}/status", timeout=3)
        return r.status_code == 200
    except:
        return False

def wait_for_server(max_wait=60):
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

def create_block():
    log("Creating block 80x60x40...")
    data = {"length": 80.0, "width": 60.0, "height": 40.0}
    try:
        r = requests.post(f"{BASE_URL}/create_block", json=data, timeout=30)
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
    log("===== Full Automatic ZW3D Test (Simplified) =====")
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
        log("ZW3D started, waiting 60 seconds for full load...")
        time.sleep(60)

    # Step 3: Wait for server
    if not wait_for_server(max_wait=60):
        log("Cannot connect to HTTP server. Please check:")
        log("1. Is ZW3D_HTTP_Server.dll loaded?")
        log("2. Check ZW3D message window for errors")
        return

    # Wait extra time for everything to stabilize
    log("Waiting extra 10 seconds for ZW3D to stabilize...")
    time.sleep(10)

    # Step 4: Create block - API will auto-create new part if needed
    if not create_block():
        log("Failed to create block")
        log("")
        log("Check ZW3D message window for detailed error information")
        return

    time.sleep(3)

    # Step 5: Generate drafting
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
