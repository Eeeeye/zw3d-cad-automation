import socket

def test_raw(request_bytes):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(('localhost', 8081))
        s.send(request_bytes)
        resp = s.recv(8192).decode()
        return resp
    except Exception as e:
        return f"Error: {e}"
    finally:
        s.close()

# Test exact format that code expects
req = b'POST /create_gear HTTP/1.1\r\nHost: localhost:8081\r\nContent-Length: 39\r\n\r\n{"teeth":20,"radius":40,"thickness":10}'
print(f"Sending: {repr(req)}")
print()
result = test_raw(req)
print(result)
print()

# Check if the code would find it
result_bytes = req + b'\x00'  # null-terminate like buffer
print(f"Contains 'POST /create_gear': {b'POST /create_gear' in result_bytes}")
print(f"Contains 'POST ': {b'POST ' in result_bytes}")
print(f"Contains '/create_gear': {b'/create_gear' in result_bytes}")
