import socket

def test_raw(method, path, body=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(('localhost', 8081))

        if body:
            req = f"{method} {path} HTTP/1.1\r\nHost: localhost:8081\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}"
        else:
            req = f"{method} {path} HTTP/1.1\r\nHost: localhost:8081\r\n\r\n"

        print(f"Request: {repr(req[:100])}")
        s.send(req.encode())
        response = s.recv(8192).decode()
        return response
    except Exception as e:
        return f"Error: {e}"
    finally:
        s.close()

print("=== Testing all endpoints ===\n")

# Test 1: Status
print("1. GET /status")
result = test_raw("GET", "/status")
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 2: Create Block
print("2. POST /create_block")
result = test_raw("POST", "/create_block", '{"length":10,"width":10,"height":10}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 3: Generate Drafting
print("3. POST /generate_drafting")
result = test_raw("POST", "/generate_drafting", '{}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 4: Test Step
print("4. GET /test_step?n=1")
result = test_raw("GET", "/test_step?n=1")
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 5: Create Gear
print("5. POST /create_gear")
result = test_raw("POST", "/create_gear", '{"teeth":20,"radius":40,"thickness":10}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 6: Create Complex
print("6. POST /create_complex")
result = test_raw("POST", "/create_complex", '{}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 7: Create Assembly
print("7. POST /create_assembly")
result = test_raw("POST", "/create_assembly", '{}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
print()

# Test 8: Execute
print("8. POST /execute")
result = test_raw("POST", "/execute", '{"command":"CvxPrtCreate"}')
parts = result.split('\r\n\r\n')
if len(parts) > 1:
    print(f"  Body: {parts[1]}")
