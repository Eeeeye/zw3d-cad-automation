#!/usr/bin/env python3
"""
Complete ZW3D PMI Feature Test

This script tests the complete workflow:
1. Start ZW3D (auto-loads DLL)
2. Create part
3. Create object (block)
4. Generate 3 views
5. Add PMI dimensions
"""

import requests
import json
import time
import subprocess
import sys
import os

# ZW3D HTTP Server configuration
ZW3D_HOST = "localhost"
ZW3D_PORT = 8081
ZW3D_BASE_URL = f"http://{ZW3D_HOST}:{ZW3D_PORT}"

# ZW3D executable path
ZW3D_EXE = r"C:\Program Files\ZWSOFT\ZW3D 2026\bin\ZW3D.exe"


def print_section(title):
    """Print a formatted section header"""
    print("\n" + "="*60)
    print(f"  {title}")
    print("="*60 + "\n")


def check_server(retries=10, delay=3):
    """Check if ZW3D HTTP server is running"""
    print(f"Checking if ZW3D HTTP server is running on port {ZW3D_PORT}...")

    for i in range(retries):
        try:
            response = requests.get(f"{ZW3D_BASE_URL}/status", timeout=2)
            if response.status_code == 200:
                print(f"✓ Server is running!")
                print(f"  Response: {response.json()}")
                return True
        except requests.exceptions.ConnectionError:
            if i < retries - 1:
                print(f"  Attempt {i+1}/{retries} failed, retrying in {delay}s...")
                time.sleep(delay)
            else:
                print(f"✗ Server not responding after {retries} attempts")
                return False
        except Exception as e:
            print(f"✗ Error: {e}")
            return False

    return False


def start_zw3d():
    """Start ZW3D application"""
    print_section("Step 1: Starting ZW3D")

    if not os.path.exists(ZW3D_EXE):
        print(f"✗ ZW3D executable not found at: {ZW3D_EXE}")
        print("  Please update ZW3D_EXE path in the script")
        return False

    print(f"Starting ZW3D from: {ZW3D_EXE}")
    print("Note: ZW3D will auto-load the DLL from:")
    print(r"  C:\Users\Ey\Desktop\claude\ZW3D_Bridge\bin\ZW3D_HTTP_Server.dll")

    try:
        # Start ZW3D process
        subprocess.Popen([ZW3D_EXE])
        print("✓ ZW3D started successfully")
        print("  Waiting for DLL to load and server to start...")
        time.sleep(5)  # Give ZW3D time to load
        return True
    except Exception as e:
        print(f"✗ Failed to start ZW3D: {e}")
        return False


def create_block(length=100, width=60, height=40):
    """Create a block in ZW3D"""
    print_section("Step 2: Creating Block")

    data = {
        "length": length,
        "width": width,
        "height": height
    }

    print(f"Creating block: {length} x {width} x {height} mm")

    try:
        response = requests.post(
            f"{ZW3D_BASE_URL}/create_block",
            json=data,
            timeout=30
        )
        result = response.json()
        print(f"✓ Block created successfully!")
        print(f"  Response: {json.dumps(result, indent=2)}")
        return True
    except Exception as e:
        print(f"✗ Failed to create block: {e}")
        return False


def generate_3views():
    """Generate 3 orthographic views"""
    print_section("Step 3: Generating 3 Views")

    print("Creating drawing sheet with 3 standard views...")

    try:
        response = requests.post(
            f"{ZW3D_BASE_URL}/generate_drafting",
            json={},
            timeout=120
        )
        result = response.json()
        print(f"✓ 3 views generated successfully!")
        print(f"  Response: {json.dumps(result, indent=2)}")
        return True
    except Exception as e:
        print(f"✗ Failed to generate views: {e}")
        return False


def add_pmi_dimensions():
    """Add PMI dimensions to views"""
    print_section("Step 4: Adding PMI Dimensions")

    print("Adding automatic dimensions to all views...")

    try:
        response = requests.post(
            f"{ZW3D_BASE_URL}/add_pmi",
            json={},
            timeout=60
        )
        result = response.json()
        print(f"✓ PMI dimensions added successfully!")
        print(f"  Response: {json.dumps(result, indent=2)}")
        return True
    except Exception as e:
        print(f"✗ Failed to add PMI dimensions: {e}")
        return False


def main():
    """Main test workflow"""
    print("\n" + "="*60)
    print("  ZW3D PMI Feature - Complete Workflow Test")
    print("="*60)

    # Check if server is already running
    server_running = check_server(retries=3, delay=1)

    # Start ZW3D if server not running
    if not server_running:
        if not start_zw3d():
            print("\n✗ Cannot proceed without ZW3D running")
            return False

        # Wait for server to be ready
        if not check_server(retries=15, delay=3):
            print("\n✗ ZW3D started but server not responding")
            print("  Please check ZW3D message window for DLL loading errors")
            return False
    else:
        print("✓ Using existing ZW3D session")

    # Step 2: Create block
    if not create_block(length=100, width=60, height=40):
        return False

    time.sleep(2)

    # Step 3: Generate 3 views
    if not generate_3views():
        return False

    time.sleep(2)

    # Step 4: Add PMI dimensions
    if not add_pmi_dimensions():
        return False

    # Success!
    print_section("Test Complete!")
    print("✓ All steps completed successfully!")
    print("\nNext steps:")
    print("  1. Check ZW3D to see the created drawing")
    print("  2. Verify 3 views are present (Front, Top, Right)")
    print("  3. Verify dimensions are added")
    print("  4. Save the drawing if needed")
    print()

    return True


if __name__ == "__main__":
    try:
        success = main()
        sys.exit(0 if success else 1)
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
