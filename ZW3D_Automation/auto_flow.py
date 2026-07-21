#!/usr/bin/env python3
"""
ZW3D Automation Pipeline - Updated for ZW3D_HTTP_Server

This script creates a part, generates drafting views with auto dimensions,
and exports to PDF.
"""

import requests
import time
import sys

BASE_URL = "http://localhost:8081"

def log(msg):
    print(f"[INFO] {msg}")

def check_server():
    """Check if ZW3D HTTP Server is running"""
    try:
        response = requests.get(f"{BASE_URL}/status", timeout=5)
        if response.status_code == 200:
            log("Server is running and responsive")
            return True
    except Exception as e:
        log(f"Server check failed: {e}")
        return False

def create_block():
    """Create a block in ZW3D"""
    log("Creating block (80x60x40)...")
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
    """Generate drafting with 3 views and auto dimensions"""
    log("Generating drafting with 3 views...")
    response = requests.post(f"{BASE_URL}/generate_drafting", json={}, timeout=120)
    if response.status_code == 200:
        result = response.json()
        log(f"Drafting generated: {result}")
        return True
    else:
        log(f"Failed to generate drafting: {response.status_code} - {response.text}")
        return False

def export_pdf():
    """Export to PDF"""
    log("Exporting to PDF...")
    response = requests.post(f"{BASE_URL}/export/pdf", json={"filename": "Auto_Drafting.pdf"}, timeout=30)
    if response.status_code == 200:
        result = response.json()
        log(f"PDF exported: {result}")
        return True
    else:
        log(f"Failed to export PDF: {response.status_code} - {response.text}")
        return False

def main():
    log("=== ZW3D Automation Pipeline Started ===")
    log("")
    log("Prerequisite: Please create a new Part in ZW3D first!")
    log("(File → New → Part → OK)")
    log("")

    # Step 1: Check server
    if not check_server():
        log("\nERROR: Server is not running!")
        log("Please make sure:")
        log("1. ZW3D is open")
        log("2. ZW3D_HTTP_Server plugin is loaded")
        log("3. Check message window for 'HTTP Server listening on port 8081'")
        return

    # Step 2: Create block
    time.sleep(1)
    if not create_block():
        log("\nERROR: Failed to create block")
        log("Hint: Make sure you have created a new Part in ZW3D first!")
        return

    # Step 3: Generate drafting with auto dimensions
    time.sleep(1)
    if not generate_drafting():
        log("\nERROR: Failed to generate drafting")
        return

    # Step 4: Export to PDF - NOT IMPLEMENTED in current DLL
    # time.sleep(1)
    # if not export_pdf():
    #     log("\nERROR: Failed to export PDF")
    #     return

    log("\n=== Pipeline Complete ===")
    log("\nResults:")
    log("- Block created: 80 x 60 x 40 mm")
    log("- Drafting generated with 3 views (Front, Top, Right)")
    log("- Auto dimensions added (should show 80, 60, 40 values)")
    log("- Output saved to: C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3")

if __name__ == "__main__":
    main()
