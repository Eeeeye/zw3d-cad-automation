#!/usr/bin/env python3
"""Test all ZW3D HTTP Server DLL endpoints"""

import socket
import time
import json

def http_request(method, path, body=None):
    """Send HTTP request and get response"""
    port = 8081
    host = 'localhost'

    headers = [
        f"{method} {path} HTTP/1.1",
        f"Host: {host}:{port}",
    ]

    if body:
        body_str = json.dumps(body)
        headers.append("Content-Type: application/json")
        headers.append(f"Content-Length: {len(body_str)}")
        headers.append("")
        headers.append(body_str)
    else:
        headers.append("")

    request = "\r\n".join(headers)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((host, port))
        sock.send(request.encode())

        response = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
        sock.close()

        response_str = response.decode()
        if "\r\n\r\n" in response_str:
            _, body = response_str.split("\r\n\r\n", 1)
            return body
        return response_str
    except Exception as e:
        return f"Error: {e}"

print("=== ZW3D HTTP Server DLL Test ===\n")

# Test 1: Status
print("Test 1: GET /status")
result = http_request("GET", "/status")
print(f"  Result: {result}\n")

# Test 2: Create Block
print("Test 2: POST /create_block")
result = http_request("POST", "/create_block", {"length": 50, "width": 30, "height": 20})
print(f"  Result: {result}\n")
time.sleep(1)

# Test 3: Create Gear
print("Test 3: POST /create_gear")
result = http_request("POST", "/create_gear", {"teeth": 20, "radius": 40, "thickness": 10})
print(f"  Result: {result}\n")
time.sleep(1)

# Test 4: Create Complex
print("Test 4: POST /create_complex")
result = http_request("POST", "/create_complex", {})
print(f"  Result: {result}\n")
time.sleep(1)

# Test 5: Create Assembly
print("Test 5: POST /create_assembly")
result = http_request("POST", "/create_assembly", {})
print(f"  Result: {result}\n")
time.sleep(1)

# Test 6: Execute
print("Test 6: POST /execute")
result = http_request("POST", "/execute", {"command": "CvxPrtCreate"})
print(f"  Result: {result}\n")

# Test 7: Test Step
print("Test 7: GET /test_step?n=1")
result = http_request("GET", "/test_step?n=1")
print(f"  Result: {result}\n")

print("=== Done ===")
