# A fake api.github.com for the issue tests: no request ever leaves the host.
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        state = self.server.state
        length = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode("utf-8"))
        except ValueError:
            body = None
        state["requests"].append({"path": self.path, "body": body,
                                  "auth": self.headers.get("authorization"),
                                  "accept": self.headers.get("accept"),
                                  "version": self.headers.get("x-github-api-version")})
        status, answer = state["answer"]
        payload = json.dumps(answer).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class FakeGitHub:
    """Answers POST /repos/{owner}/{repo}/issues."""

    def __init__(self, number=41, repo="Franckrst/D2Vita"):
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.state = {"requests": [], "answer": (201, {
            "number": number,
            "html_url": f"https://github.com/{repo}/issues/{number}",
        })}
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    @property
    def base_url(self):
        host, port = self.httpd.server_address[:2]
        return f"http://{host}:{port}"

    @property
    def requests(self):
        return self.httpd.state["requests"]

    def answer_with(self, status, body):
        self.httpd.state["answer"] = (status, body)

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)
