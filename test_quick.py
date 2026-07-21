import socket
import time

def test_endpoint(method, path, body=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect(('localhost', 8081))

        if body:
            req = f"{method} {path} HTTP/1.1\r\nHost: localhost:8081\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}"
        else:
            req = f"{method} {path} HTTP/1.1\r\nHost: localhost:8081\r\n\r\n"

        s.send(req.encode())
        response = s.recv(4096).decode()
        return response
    except Exception as e:
        return f"Error: {e}"
    finally:
        s.close()

# Quick test for each endpoint
endpoints = [
    ("GET", "/status"),
    ("POST", "/create_block", '{"length":10,"width":10,"height":10}'),
    ("POST", "/create_gear", '{"teeth":20,"radius":40,"thickness":10}'),
    ("POST", "/create_complex", '{}'),
    ("POST", "/create_assembly", '{}'),
    ("POST", "/execute", '{"command":"CvxBox 10 10 10"}'),
]

for test in endpoints:
    if len(test) == 2:
        method, path = test
        body = None
    else:
        method, path, body = test

    print(f"\n{method} {path}")
    result = test_endpoint(method, path, body)
    print(result[:200] if len(result) > 200 else result)

    # Extract JSON body from HTTP response
    if "\r\n\r\n" in result:
        json_body = result.split("\r\n\r\n")[1]
        print(f"JSON: {json_body}")
