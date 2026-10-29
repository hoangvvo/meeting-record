"""HTTPS fixture server; the isolated test browser resolves test hosts locally."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import ssl
import sys

runtime, fixture = map(Path, sys.argv[1:])


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = fixture.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = HTTPServer(("127.0.0.1", 9443), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(runtime / "cert.pem", runtime / "key.pem")
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
