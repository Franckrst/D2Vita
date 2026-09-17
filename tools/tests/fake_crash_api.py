#!/usr/bin/env python3
"""tools/tests/fake_crash_api.py — scripted stand-in for the crash-report API.

Used by tools/tests/crashreport_transport_test.cpp to drive the
console side of the exchange without Cloudflare:

  --mode api  implements the console routes of the v1 contract
              (D2Vita-website contract/README.md): POST /v1/claims,
              PUT /v1/reports/{id}/artifacts/{name}, POST
              /v1/reports/{id}/complete. Answers are signed with an Ed25519
              seed from contract/vectors/response-sig.v1.json, pieces are
              opened with an X25519 secret key from sealed.v1.json, and every
              request is checked against the contract: the two console
              headers, Content-Length, the upload token, the requested pieces
              and their caps. What the console breaks is written in the log as
              a violation, never hidden.

  --mode raw  replies with exact bytes, split and delayed as the script says:
              chunked bodies cut in pieces, chunk extensions, oversized heads,
              truncated bodies, connections cut mid-answer.

A script is a JSON file (--script); every request is appended to the log
(--log, one JSON object per line) and the port the server listens on is
written to --port-file once it is bound, so the test can start it and wait.

Overrides (api mode) are consumed in order: each one matches a route
(claim, artifact, complete, any) and changes what that request gets.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

try:
    import nacl.bindings as sodium
    import nacl.exceptions
    import nacl.signing
except ImportError:  # pragma: no cover - the test script checks for PyNaCl first
    sys.stderr.write("fake_crash_api.py needs PyNaCl\n")
    raise

MAGIC = b"D2VSEAL1"
HEADER_SIZE = 72
TAG_SIZE = 16
NONCE_PREFIX_SIZE = 16

ARTIFACT_CAPS = {
    "dump": 2 * 1024 * 1024,
    "crash_txt": 64 * 1024,
    "crash_log": 64 * 1024,
    "boot_progress": 320 * 1024,
}
CLAIM_MAX_BYTES = 16 * 1024


# --------------------------------------------------------------------------
# D2VSEAL1 opening (contract/sealed-format.md section 6)
# --------------------------------------------------------------------------

def open_sealed(blob, recipient_sk):
    if len(blob) < HEADER_SIZE:
        raise ValueError("truncated")
    header = blob[:HEADER_SIZE]
    (chunk_size,) = struct.unpack_from("<I", header, 64)
    if header[:8] != MAGIC or header[68:72] != bytes(4) or not 1 <= chunk_size <= 1048576:
        raise ValueError("malformed")
    recipient_pk = sodium.crypto_scalarmult_base(recipient_sk)
    key_id = hashlib.blake2b(recipient_pk, digest_size=32).digest()[:8]
    if header[8:16] != key_id:
        raise ValueError("wrong_key")
    eph_pk = header[16:48]
    try:
        shared = sodium.crypto_scalarmult(recipient_sk, eph_pk)
    except RuntimeError:
        shared = bytes(32)
    if shared == bytes(32):
        raise ValueError("malformed")
    key = hashlib.blake2b(MAGIC + shared + eph_pk + recipient_pk, digest_size=32).digest()
    body = blob[HEADER_SIZE:]
    full = chunk_size + TAG_SIZE
    if len(body) % full < TAG_SIZE:
        raise ValueError("truncated")
    count = len(body) // full + 1
    out = []
    for index in range(count):
        last = index == count - 1
        chunk = body[index * full:] if last else body[index * full:(index + 1) * full]
        nonce = header[48:64] + struct.pack("<Q", index)
        try:
            out.append(sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(
                chunk, header + bytes([1 if last else 0]), nonce, key))
        except nacl.exceptions.CryptoError:
            raise ValueError("tampered") from None
    return b"".join(out)


def seal(plaintext, recipient_pk, eph_sk, nonce_prefix, chunk_size=65536):
    """The writing side of the same format (contract/sealed-format.md)."""
    eph_pk = sodium.crypto_scalarmult_base(eph_sk)
    shared = sodium.crypto_scalarmult(eph_sk, recipient_pk)
    key = hashlib.blake2b(MAGIC + shared + eph_pk + recipient_pk, digest_size=32).digest()
    header = (MAGIC + hashlib.blake2b(recipient_pk, digest_size=32).digest()[:8] + eph_pk + nonce_prefix +
              struct.pack("<I", chunk_size) + bytes(4))
    out, offset = [header], 0
    count = len(plaintext) // chunk_size + 1
    for index in range(count):
        last = index == count - 1
        chunk = plaintext[offset:] if last else plaintext[offset:offset + chunk_size]
        offset += len(chunk)
        out.append(sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(
            chunk, header + bytes([1 if last else 0]), nonce_prefix + struct.pack("<Q", index), key))
    return b"".join(out)


# --------------------------------------------------------------------------
# HTTP plumbing
# --------------------------------------------------------------------------

class Cut(Exception):
    """The script asks for the connection to be cut here."""


def recv_head(conn, limit=65536):
    data = b""
    while b"\r\n\r\n" not in data:
        if len(data) > limit:
            raise Cut("head too large")
        piece = conn.recv(4096)
        if not piece:
            return None, b""
        data += piece
    head, rest = data.split(b"\r\n\r\n", 1)
    return head, rest


def parse_head(head):
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) < 3:
        raise Cut("bad request line")
    headers = {}
    for line in lines[1:]:
        if not line:
            continue
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return parts[0], parts[1], parts[2], headers


def send_all(conn, data):
    try:
        conn.sendall(data)
    except OSError:
        raise Cut("client went away") from None


def http_response(status, reason, body, extra_headers=(), chunked=False, chunk_pieces=1, extensions=False):
    """Bytes of a response; chunked splits the body into chunk_pieces chunks."""
    head = "HTTP/1.1 %d %s\r\n" % (status, reason)
    for name, value in extra_headers:
        head += "%s: %s\r\n" % (name, value)
    head += "Connection: close\r\n"
    if chunked:
        head += "Transfer-Encoding: chunked\r\n\r\n"
        out = head.encode()
        step = max(1, (len(body) + chunk_pieces - 1) // chunk_pieces) if body else 0
        pieces = [body[i:i + step] for i in range(0, len(body), step)] if body else []
        for piece in pieces:
            ext = ";name=d2v" if extensions else ""
            out += ("%x%s\r\n" % (len(piece), ext)).encode() + piece + b"\r\n"
        out += b"0\r\n\r\n"
        return out
    head += "Content-Length: %d\r\n\r\n" % len(body)
    return head.encode() + body


# --------------------------------------------------------------------------
# The server
# --------------------------------------------------------------------------

class Server:
    def __init__(self, script, log_path, artifacts_dir):
        self.script = script
        self.mode = script.get("mode", "api")
        self.log_path = log_path
        self.artifacts_dir = artifacts_dir
        self.seq = 0
        self.overrides = list(script.get("overrides", []))
        self.exchanges = list(script.get("exchanges", []))
        self.grants = {}          # token -> {report_id, artifacts: {name: max_bytes}}
        self.stored = {}          # report_id -> {name: {bytes, sha256}}
        self.claims = {}          # report_id -> decision text
        seed = script.get("signing_seed_hex")
        self.signer = nacl.signing.SigningKey(bytes.fromhex(seed)) if seed else None
        other = script.get("other_signing_seed_hex")
        self.other_signer = nacl.signing.SigningKey(bytes.fromhex(other)) if other else None
        sk = script.get("recipient_sk_hex")
        self.recipient_sk = bytes.fromhex(sk) if sk else None
        if artifacts_dir:
            os.makedirs(artifacts_dir, exist_ok=True)

    # ---------------------------------------------------------------- log
    def log(self, entry):
        entry["seq"] = self.seq
        with open(self.log_path, "a") as f:
            f.write(json.dumps(entry, sort_keys=True) + "\n")
            f.flush()

    def flush(self, entry):
        """Writes the entry once, BEFORE the answer leaves.

        The test kills the server as soon as its last answer is in: logging
        after sending would race with that signal.
        """
        if entry.get("_logged"):
            return
        entry["_logged"] = True
        self.log({k: v for k, v in entry.items() if k != "_logged"})

    # ------------------------------------------------------------ signing
    def sign(self, body, override):
        if override.get("unsigned"):
            return None
        signer = self.other_signer if override.get("bad_signature") and self.other_signer else self.signer
        if signer is None:
            return None
        sig = base64.b64encode(signer.sign(body).signature).decode()
        if override.get("bad_signature") and signer is self.signer:
            flipped = bytearray(base64.b64decode(sig))
            flipped[0] ^= 0x01
            sig = base64.b64encode(bytes(flipped)).decode()
        return sig

    def answer(self, conn, status, reason, payload, override, entry):
        body = json.dumps(payload, separators=(",", ":")).encode()
        headers = []
        sig = self.sign(body, override)
        if sig is not None:
            headers.append(("X-D2V-Signature", sig))
        headers.append(("Content-Type", "application/json; charset=utf-8"))
        entry["status"] = status
        entry["answer"] = payload
        entry["signed"] = sig is not None
        raw = http_response(status, reason, body, headers,
                            chunked=bool(override.get("chunked")),
                            chunk_pieces=int(override.get("chunk_pieces", 3)),
                            extensions=bool(override.get("chunk_extensions")))
        cut = override.get("cut")
        entry["cut"] = cut
        self.flush(entry)
        if cut == "before_answer":
            raise Cut("before the answer")
        if cut == "after_headers":
            head = raw.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n"
            send_all(conn, head)
            raise Cut("after the headers")
        if cut == "mid_body":
            send_all(conn, raw[:len(raw) - max(1, len(body) // 2)])
            raise Cut("mid body")
        if override.get("delay_ms"):
            time.sleep(override["delay_ms"] / 1000.0)
        send_all(conn, raw)

    # ------------------------------------------------------------ routing
    def take_override(self, route):
        for i, o in enumerate(self.overrides):
            if o.get("route") in (route, "any", None):
                left = int(o.get("count", 1)) - 1
                if left <= 0:
                    self.overrides.pop(i)
                else:
                    o["count"] = left
                return o
            if o.get("route") == "none":       # placeholder that never matches
                continue
            break                              # overrides apply in order
        return {}

    def handle(self, conn):
        self.seq += 1
        head, rest = recv_head(conn)
        if head is None:
            self.log({"event": "empty_connection"})
            return
        method, path, version, headers = parse_head(head)
        entry = {"method": method, "path": path, "version": version, "headers": headers, "violations": []}
        declared = headers.get("content-length")
        body = rest
        route = self.route_of(method, path)
        entry["route"] = route
        override = self.take_override(route) if self.mode == "api" else {}
        if self.mode == "raw":
            override = self.exchanges.pop(0) if self.exchanges else {}
        try:
            if declared is None:
                entry["violations"].append("no Content-Length")
            else:
                want = int(declared)
                entry["content_length"] = want
                if override.get("cut") == "on_request_body":
                    read_at_most = max(0, want // 2)
                    while len(body) < read_at_most:
                        piece = conn.recv(min(4096, read_at_most - len(body)))
                        if not piece:
                            break
                        body += piece
                    entry["body_bytes"] = len(body)
                    entry["cut"] = "on_request_body"
                    raise Cut("while reading the body")
                if override.get("answer_before_body") and want > len(body):
                    entry["answered_before_body"] = True
                else:
                    while len(body) < want:
                        piece = conn.recv(min(65536, want - len(body)))
                        if not piece:
                            break
                        body += piece
                entry["body_bytes"] = len(body)
                entry["body_complete"] = len(body) == want
                if len(body) != want and not entry.get("answered_before_body"):
                    entry["violations"].append("body shorter than Content-Length")
            if self.mode == "raw":
                self.raw_answer(conn, override, entry)
            else:
                self.api_answer(conn, method, path, headers, body, override, entry)
        except Cut as e:
            entry["cut_reason"] = str(e)        # only useful when nothing was logged yet
        finally:
            self.flush(entry)

    def route_of(self, method, path):
        if method == "POST" and path.endswith("/v1/claims"):
            return "claim"
        if method == "PUT" and "/artifacts/" in path:
            return "artifact"
        if method == "POST" and path.endswith("/complete"):
            return "complete"
        return "other"

    # ---------------------------------------------------------------- raw
    def raw_answer(self, conn, exchange, entry):
        entry["sent_pieces"] = len(exchange.get("send", []))
        self.flush(entry)
        if exchange.get("delay_ms"):
            time.sleep(exchange["delay_ms"] / 1000.0)
        for piece in exchange.get("send", []):
            if "delay_ms" in piece:
                time.sleep(piece["delay_ms"] / 1000.0)
                continue
            data = piece["text"].encode("latin-1") if "text" in piece else bytes.fromhex(piece["hex"])
            if piece.get("repeat"):
                data = data * int(piece["repeat"])
            send_all(conn, data)
        if exchange.get("hang_ms"):
            time.sleep(exchange["hang_ms"] / 1000.0)

    # ---------------------------------------------------------------- api
    def api_answer(self, conn, method, path, headers, body, override, entry):
        route = entry["route"]
        if route == "artifact":                 # named even when an override answers
            entry["report_id"] = self.report_of(path, body)
            entry["artifact"] = path.rsplit("/", 1)[-1]
        elif route == "complete":
            entry["report_id"] = self.report_of(path, body)
        if override.get("status"):
            payload = {"v": 1, "error": override.get("error", "internal_error"),
                       "message": override.get("message", "scripted answer")}
            for key in ("retry_after_s", "disable_until_unix"):
                if key in override:
                    payload[key] = override[key]
            if override.get("bind_report"):
                payload["report_id"] = self.report_of(path, body)
            if override.get("bind_artifact"):
                payload["artifact"] = path.rsplit("/", 1)[-1]
            for key, value in override.get("extra_fields", {}).items():
                payload[key] = value
            reason = {400: "Bad Request", 403: "Forbidden", 409: "Conflict", 413: "Payload Too Large",
                      429: "Too Many Requests", 500: "Internal Server Error",
                      503: "Service Unavailable"}.get(override["status"], "Error")
            self.answer(conn, override["status"], reason, payload, override, entry)
            return
        if route == "claim":
            self.handle_claim(conn, headers, body, override, entry)
        elif route == "artifact":
            self.handle_artifact(conn, path, headers, body, override, entry)
        elif route == "complete":
            self.handle_complete(conn, path, headers, body, override, entry)
        else:
            self.answer(conn, 404, "Not Found", {"v": 1, "error": "not_found", "message": "no such route"},
                        override, entry)

    def report_of(self, path, body):
        if "/v1/reports/" in path:
            return path.split("/v1/reports/", 1)[1].split("/", 1)[0]
        try:
            return json.loads(body.decode()).get("report_id", "")
        except Exception:
            return ""

    def handle_claim(self, conn, headers, body, override, entry):
        try:
            claim = json.loads(body.decode("utf-8"))
        except Exception:
            entry["violations"].append("claim is not JSON")
            self.answer(conn, 400, "Bad Request", {"v": 1, "error": "invalid_payload", "message": "not JSON"},
                        override, entry)
            return
        entry["claim"] = claim
        if len(body) > CLAIM_MAX_BYTES:
            entry["violations"].append("claim over 16 KiB")
        if headers.get("x-d2v-client") != "d2vita/%s" % claim.get("build_id"):
            entry["violations"].append("X-D2V-Client does not match build_id")
        if headers.get("x-d2v-install") != claim.get("install_id"):
            entry["violations"].append("X-D2V-Install does not match install_id")
        for name, value in (("v", 1),):
            if claim.get(name) != value:
                entry["violations"].append("claim %s is not %r" % (name, value))
        report_id = claim.get("report_id", "")
        if report_id in self.claims:
            entry["replay"] = True
            body_text = self.claims[report_id]
            self.answer_text(conn, 200, "OK", body_text, override, entry)
            return
        offered = {a["name"]: a["bytes"] for a in claim.get("artifacts", [])}
        for name, size in offered.items():
            if size > ARTIFACT_CAPS.get(name, 0):
                entry["violations"].append("offered %s over its cap" % name)
        wanted = [n for n in override.get("want", list(offered.keys())) if n in offered]
        action = override.get("action", "upload" if wanted else "count_only")
        upload = None
        if action == "upload":
            token = "tok-%s" % report_id[-8:]
            upload = {"token": token,
                      "expires_unix": int(override.get("expires_unix", time.time() + 1800)),
                      "artifacts": [{"name": n, "max_bytes": ARTIFACT_CAPS[n]} for n in wanted]}
            self.grants[token] = {"report_id": report_id, "artifacts": {n: ARTIFACT_CAPS[n] for n in wanted}}
        payload = {"v": 1,
                   "report_id": override.get("report_id", report_id),
                   "signature": override.get("signature", "SZYGIRBIXGHOM3AH"),
                   "action": action,
                   "upload": upload,
                   "retry_after_s": override.get("retry_after_s"),
                   "disable_until_unix": override.get("disable_until_unix")}
        for key, value in override.get("extra_fields", {}).items():
            payload[key] = value
        text = json.dumps(payload, separators=(",", ":")).encode()
        self.claims[report_id] = text
        self.answer_text(conn, 200, "OK", text, override, entry, payload)

    def answer_text(self, conn, status, reason, body, override, entry, payload=None):
        headers = []
        sig = self.sign(body, override)
        if sig is not None:
            headers.append(("X-D2V-Signature", sig))
        headers.append(("Content-Type", "application/json; charset=utf-8"))
        entry["status"] = status
        entry["signed"] = sig is not None
        entry["answer"] = payload if payload is not None else json.loads(body.decode())
        raw = http_response(status, reason, body, headers,
                            chunked=bool(override.get("chunked")),
                            chunk_pieces=int(override.get("chunk_pieces", 3)),
                            extensions=bool(override.get("chunk_extensions")))
        cut = override.get("cut")
        entry["cut"] = cut
        self.flush(entry)
        if cut == "before_answer":
            raise Cut("before the answer")
        if cut == "after_headers":
            send_all(conn, raw.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n")
            raise Cut("after the headers")
        if cut == "mid_body":
            send_all(conn, raw[:len(raw) - max(1, len(body) // 2)])
            raise Cut("mid body")
        if override.get("delay_ms"):
            time.sleep(override["delay_ms"] / 1000.0)
        send_all(conn, raw)

    def handle_artifact(self, conn, path, headers, body, override, entry):
        report_id = self.report_of(path, body)
        name = path.rsplit("/", 1)[-1]
        entry["artifact"] = name
        entry["report_id"] = report_id
        auth = headers.get("authorization", "")
        token = auth[len("D2V-Upload "):] if auth.startswith("D2V-Upload ") else ""
        grant = self.grants.get(token)
        if not grant:
            entry["violations"].append("upload token missing or unknown")
            self.answer(conn, 403, "Forbidden", {"v": 1, "error": "bad_token", "message": "no token"}, override, entry)
            return
        if grant["report_id"] != report_id:
            entry["violations"].append("token is for another report")
        if name not in grant["artifacts"]:
            entry["violations"].append("piece was not requested")
        cap = grant["artifacts"].get(name, ARTIFACT_CAPS.get(name, 0))
        if entry.get("content_length", 0) > cap:
            entry["violations"].append("piece over max_bytes")
        if self.recipient_sk:
            try:
                plain = open_sealed(body, self.recipient_sk)
                entry["plain_bytes"] = len(plain)
                entry["plain_sha256"] = hashlib.sha256(plain).hexdigest()
                entry["open"] = "ok"
                if self.artifacts_dir:
                    with open(os.path.join(self.artifacts_dir, "%s.%s" % (report_id, name)), "wb") as f:
                        f.write(plain)
            except ValueError as e:
                entry["open"] = str(e)
                entry["violations"].append("sealed piece does not open: %s" % e)
        already = self.stored.setdefault(report_id, {})
        if name in already:
            self.answer(conn, 409, "Conflict",
                        {"v": 1, "error": "exists", "message": "already stored",
                         "report_id": report_id, "artifact": name}, override, entry)
            return
        already[name] = {"bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()}
        payload = {"v": 1, "report_id": report_id, "name": name, "bytes": entry.get("content_length", len(body))}
        for key, value in override.get("extra_fields", {}).items():
            payload[key] = value
        self.answer(conn, 201, "Created", payload, override, entry)

    def handle_complete(self, conn, path, headers, body, override, entry):
        report_id = self.report_of(path, body)
        entry["report_id"] = report_id
        auth = headers.get("authorization", "")
        token = auth[len("D2V-Upload "):] if auth.startswith("D2V-Upload ") else ""
        grant = self.grants.get(token)
        if not grant or grant["report_id"] != report_id:
            entry["violations"].append("complete without a valid token")
            self.answer(conn, 403, "Forbidden", {"v": 1, "error": "bad_token", "message": "no token"}, override, entry)
            return
        try:
            payload = json.loads(body.decode())
            entry["complete_body"] = payload
        except Exception:
            entry["violations"].append("complete body is not JSON")
            self.answer(conn, 400, "Bad Request", {"v": 1, "error": "invalid_payload", "message": "not JSON"},
                        override, entry)
            return
        listed = payload.get("artifacts", [])
        if payload.get("v") != 1 or not isinstance(listed, list) or not listed:
            entry["violations"].append("complete body does not match CompleteRequest")
        for name in listed:
            if name not in grant["artifacts"]:
                entry["violations"].append("complete lists %s, which was not requested" % name)
        missing = [n for n in grant["artifacts"] if n not in self.stored.get(report_id, {})]
        if missing:
            self.answer(conn, 409, "Conflict",
                        {"v": 1, "error": "incomplete", "message": "pieces missing", "report_id": report_id},
                        override, entry)
            return
        self.answer(conn, 200, "OK", {"v": 1, "report_id": report_id, "sample_stored": True}, override, entry)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--artifacts")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--max-seconds", type=float, default=120.0)
    a = ap.parse_args()
    with open(a.script) as f:
        script = json.load(f)
    server = Server(script, a.log, a.artifacts)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((a.host, int(script.get("port", 0))))
    sock.listen(16)
    port = sock.getsockname()[1]
    tmp = a.port_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(port))
    os.replace(tmp, a.port_file)
    open(a.log, "a").close()
    deadline = time.time() + a.max_seconds
    sock.settimeout(1.0)
    while time.time() < deadline:
        try:
            conn, _ = sock.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        conn.settimeout(20.0)
        try:
            server.handle(conn)
        except (OSError, Cut):
            pass
        except Exception as e:                      # never die on a bad request
            server.log({"event": "error", "error": repr(e)})
        finally:
            try:
                conn.close()
            except OSError:
                pass
    sock.close()


if __name__ == "__main__":
    main()
