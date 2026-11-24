import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw)
        except ValueError:
            body = {}
        if self.path.endswith("/chat/completions"):
            if self.headers.get("Authorization") != "Bearer testkey":
                self.reply(401, {"error": {"message": "bad key"}})
                return
            messages = body.get("messages") or [{}]
            prompt = messages[0].get("content", "")
            self.reply(200, {
                "choices": [{"message": {"role": "assistant",
                                         "content": "openai:" + prompt}}]
            })
            return
        if self.path.endswith("/messages"):
            if self.headers.get("x-api-key") != "testkey":
                self.reply(401, {"type": "error",
                                 "error": {"message": "bad key"}})
                return
            messages = body.get("messages") or [{}]
            prompt = messages[0].get("content", "")
            if prompt.startswith("think:"):
                self.reply(200, {
                    "content": [
                        {"type": "thinking", "text": "hidden"},
                        {"type": "text", "text": "claude:" + prompt},
                    ]
                })
                return
            self.reply(200, {
                "content": [{"type": "text", "text": "claude:" + prompt}]
            })
            return
        self.reply(404, {"error": {"message": "no route"}})

    def reply(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = HTTPServer(("127.0.0.1", 0), Handler)
with open(sys.argv[1], "w") as port_file:
    port_file.write(str(server.server_address[1]))
server.serve_forever()
