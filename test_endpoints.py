import socket

def test(name, req_bytes):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(('localhost', 8081))
        s.send(req_bytes)
        resp = s.recv(4096).decode()
        body = resp.split('\r\n\r\n')[1] if '\r\n\r\n' in resp else resp
        print(f'{name}: {body}')
    except Exception as e:
        print(f'{name}: Error - {e}')
    finally:
        s.close()

# Test with exact format
test('create_gear', b'POST /create_gear HTTP/1.1\r\nHost: localhost:8081\r\nContent-Length: 39\r\n\r\n{"teeth":20,"radius":40,"thickness":10}')
test('create_complex', b'POST /create_complex HTTP/1.1\r\nHost: localhost:8081\r\nContent-Length: 2\r\n\r\n{}')
test('create_assembly', b'POST /create_assembly HTTP/1.1\r\nHost: localhost:8081\r\nContent-Length: 2\r\n\r\n{}')
