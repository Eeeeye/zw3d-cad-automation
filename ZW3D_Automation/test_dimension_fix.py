#!/usr/bin/env python3
"""
Test script for ZW3D HTTP Server - tests auto dimension fix
"""

import requests
import time
import json

BASE_URL = "http://localhost:8081"

def log(msg):
    print(f"[TEST] {msg}")

def check_server():
    """Check if server is running"""
    try:
        response = requests.get(f"{BASE_URL}/status", timeout=5)
        if response.status_code == 200:
            log("Server is running and responsive")
            return True
    except Exception as e:
        log(f"Server check failed: {e}")
        return False

def create_block():
    """Create a test block"""
    log("=== Creating test block ===")
    data = {
        "length": 80.0,
        "width": 60.0,
        "height": 40.0
    }

    response = requests.post(f"{BASE_URL}/create_block", json=data, timeout=30)
    if response.status_code == 200:
        result = response.json()
        log(f"Block created: {result}")
        return True
    else:
        log(f"Failed to create block: {response.status_code} - {response.text}")
        return False

def generate_drafting():
    """Generate drafting with auto dimensions"""
    log("=== Generating drafting with auto dimensions ===")
    log("This tests the fix: includeAuto = ZW_DIMENSION_AUTO_INCLUDE_VALID")

    response = requests.post(f"{BASE_URL}/generate_drafting", json={}, timeout=120)

    if response.status_code == 200:
        result = response.json()
        log(f"Drafting generated: {result}")
        return True
    else:
        log(f"Failed to generate drafting: {response.status_code} - {response.text}")
        return False

def main():
    log("=== ZW3D Auto Dimension Fix Test ===")
    log("")

    # Step 1: Check server
    if not check_server():
        log("\nERROR: Server is not running!")
        log("Please make sure:")
        log("1. ZW3D is open")
        log("2. ZW3D_HTTP_Server plugin is loaded")
        log("3. Check ZW3D message window for 'HTTP Server listening on port 8081'")
        return

    # Step 2: Wait a bit
    time.sleep(1)

    # Step 3: Create block
    if not create_block():
        log("\nERROR: Failed to create block")
        return

    time.sleep(2)

    # Step 4: Generate drafting with auto dimensions
    if not generate_drafting():
        log("\nERROR: Failed to generate drafting")
        return

    log("\n=== Test Complete ===")
    log("\nPlease check:")
    log("1. The drawing should have 3 views (Front, Top, Right)")
    log("2. Each view should have auto dimensions showing correct values")
    log("3. Dimensions should show 80, 60, and 40 values")

if __name__ == "__main__":
    main()
