"""Loopback-only, disk-backed WebDAV fixture (Python standard library only).

The runner owns the directory and lifetime; import WebDavFixture to drive it from
another Python orchestrator. DAV lives at /notebook/, control at /__control. All
control requests are POST application/json with Authorization: Bearer <token>.
Success is HTTP 200 JSON {"ok": true, ...}; invalid controls fail with 400. Neither
HTTP logging nor exception reporting prints headers, credentials, or bodies.

Control protocol (paths are decoded notebook-relative UTF-8, root is ""):
  {"action":"reset"} clears remote files, configuration, faults, barriers and
    counters, restores the original credentials, and invalidates in-flight writes.
    The immutable strict(default)/jianguoyun profile is preserved across reset.
  {"action":"configure", ...} sets any of: auth="basic"|"digest"|"both",
    username/password, anonymous_read=false (only GET/PROPFIND/OPTIONS),
    namespace="prefix"|"default", namespace_prefix="D", href_mode="path"|
    "absolute"|"relative", etag_mode="strong"|"weak"|"missing",
    ignore_create_conditions/ignore_move_conditions/ignore_delete_conditions=false.
    These last three deliberately emulate unsafe servers for capability probes.
  {"action":"put","path":"dir/file", "text":"UTF-8"} writes ordinary bytes;
    alternatively content_base64, or size + repeat_base64 (default: zero bytes)
    streams large fixtures without a large JSON allocation. parents=true creates
    missing parents. Returns metadata, including exact profile etag and sha256.
  {"action":"mkdir","path":"dir", "parents":true} creates collections.
  {"action":"delete","path":"file", "recursive":false} removes a resource;
    recursive=true is an EXTERNAL-client control only, never a DAV DELETE.
  {"action":"touch_etag","path":"file"} changes the strong ETag, not bytes.
  {"action":"inspect","path":"file", "content":false} returns metadata;
    content=true includes content_base64 only for files <= 1 MiB. Absent => exists=false.
  {"action":"tree","offset":0,"limit":1000} returns a sorted metadata page.
  {"action":"requests","since":0,"limit":1000} returns monotonically numbered
    method/path/status/authenticated records, method/status counters and oldest sequence.
    At most 10,000 records are retained; no request headers or bodies are retained.
  {"action":"fault","method":"PROPFIND","path":"", "effect":"xml", ...}
    arms a rule. method/path default "*"; phase="before"|"after", count=1
    (count=-1 means persistent). Matching authorized requests consume rules.
    Optional path_prefix/destination (decoded MOVE target)/depth narrow matching; skip=N
    ignores the first N matching requests, permitting deterministic quota windows.
    effects: status + status (400..599); redirect + location (+ status default 301);
    xml + body (literal UTF-8), or size + repeat_base64; delay + delay_ms;
    drop (close without acknowledgement); truncate + bytes (advertise full size,
    then close early); throttle + delay_ms (per 64 KiB response chunk);
    weak_etag/missing_etag (headers AND generated DAV properties); the jianguoyun
    profile additionally accepts raw_etag/quoted_etag for validator-negative cases.
    xml replaces a PROPFIND response with HTTP 207; it can express DTDs, bad hrefs,
    duplicates, partial/failed propstats, alias collisions, and oversized XML.
    xml + resource_count=N emits a real collection's self response plus N synthetic
    file children, in compact streamed XML for the global scan-resource limit.
    These injected children are not stored; any GET still honestly returns 404.
    after effects run AFTER the normal operation: drop emulates a committed write
    with a lost acknowledgement. Failure injection is never reported as success.
    status/redirect/xml also accept headers (at most 16 KiB, no framing headers).
    phase="after_headers" is available for delay/drop/barrier to stall a GET body.
  {"action":"barrier","id":"race","method":"MOVE","path":"scratch",
    "phase":"before","count":1} pauses matching requests before precondition
    evaluation or after mutation/before response headers. No store lock is held.
    Jianguoyun-only drop_after_release=true disconnects that same request after
    releasing its barrier, allowing a descendant before a lost acknowledgement.
  {"action":"wait","id":"race","timeout_ms":5000} => reached + hits.
  {"action":"release","id":"race"} releases the barrier permanently.
  {"action":"clear_faults"} clears rules and releases/removes all barriers.

Defaults require real Basic or Digest authentication for EVERY DAV method. Digest
uses MD5/qop=auth, random nonce/opaque, exact request-target validation, and replay
checks. Selecting anonymous_read models an intentionally public read-only server;
there is no unconditional authentication bypass. Control auth is always separate.

PUT requires If-None-Match:* or exact If-Match. MOVE requires Overwrite:F for an
absent destination, or Overwrite:T plus tagged If: <destination-URL> (["etag"])
for an existing destination. DELETE requires exact If-Match and accepts files only.
All conditions are checked together with the mutation under the store lock.

Explicit profile="jianguoyun" models the observed provider instead: raw GET/DAV
validators, no PUT ETag, exact raw PUT If-Match, ignored GET/PUT If-None-Match/
DELETE/source-MOVE conditions, create-only MOVE (409 occupied, 201 absent), and
idempotent MKCOL201 with file collisions rejected. Depth:1 returns at most 750
real resources including self; it is deliberately NOT a complete inventory.
Control tree pagination remains complete. Profile cannot be changed by controls.

Payloads, hashes, uploads, downloads and XML spool files use 64 KiB chunks. Control
JSON is bounded at 1 MiB, ordinary request XML at 64 KiB, files at 8 GiB, injected
XML at 64 MiB, metadata at 300,000 resources, faults/barriers at 256 each, and
simultaneous requests at 32. A bound returns an error, never truncated success.
The generated listing includes the queried collection and its immediate children.
Filesystem paths are containment-checked and never traverse symlinks/reparse points.
"""

import base64
import binascii
from collections import Counter, deque
from email.utils import formatdate
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import posixpath
import re
import secrets
import shutil
import socket
import ssl
import stat
import tempfile
import threading
import time
from urllib.parse import quote, unquote_to_bytes, urlsplit
from urllib.request import parse_http_list, parse_keqv_list
from xml.sax.saxutils import escape

CHUNK = 64 * 1024
MAX_CONTROL = 1024 * 1024
MAX_FILE = 8 * 1024 * 1024 * 1024
MAX_XML = 64 * 1024 * 1024
MAX_RESOURCES = 300000
REALM = "vxcore-webdav-fixture"
METHODS = {"OPTIONS", "PROPFIND", "GET", "PUT", "MKCOL", "MOVE", "DELETE"}
DEFAULT_CONFIG = {
    "auth": "both", "anonymous_read": False, "namespace": "prefix",
    "namespace_prefix": "D", "href_mode": "path", "etag_mode": "strong",
    "ignore_create_conditions": False, "ignore_move_conditions": False,
    "ignore_delete_conditions": False,
}


class DigestNonceExpired(Exception):
    """A valid Digest request must authenticate again against the new bounded epoch."""


class DavError(Exception):
    def __init__(self, status):
        self.status = status


def integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise DavError(400)
    return value


def relative_path(value):
    if not isinstance(value, str) or len(value.encode("utf-8")) > 32768:
        raise DavError(400)
    if value == "":
        return value
    parts = value.split("/")
    for part in parts:
        if (not part or part in (".", "..") or part[-1:] in (".", " ")
                or any(ord(c) < 32 or c in '\\:*?"<>|\x7f' for c in part)
                or re.fullmatch(r"(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", part)):
            raise DavError(400)
    return value


def decode_url_path(raw):
    if not raw.startswith("/") or re.search(r"%(?![0-9a-fA-F]{2})", raw):
        raise DavError(400)
    parts = []
    for component in raw.split("/")[1:]:
        try:
            decoded = unquote_to_bytes(component).decode("utf-8", "strict")
        except UnicodeError:
            raise DavError(400) from None
        if "/" in decoded or "\\" in decoded:
            raise DavError(400)
        parts.append(decoded)
    if parts and parts[-1] == "":
        parts.pop()
    if not parts or parts[0] != "notebook":
        raise DavError(404)
    return relative_path("/".join(parts[1:]))


def bytes_spec(data, maximum):
    """Return a bounded repeating seed and total length, never a large payload."""
    choices = sum(key in data for key in ("text", "content_base64", "size"))
    if choices > 1:
        raise DavError(400)
    try:
        if "size" in data:
            size = integer(data["size"], 0, maximum)
            seed = base64.b64decode(data.get("repeat_base64", "AA=="), validate=True)
            if not seed or len(seed) > CHUNK:
                raise DavError(400)
            return seed, size
        if "content_base64" in data:
            content = base64.b64decode(data["content_base64"], validate=True)
        else:
            content = data.get("text", "").encode("utf-8")
    except (ValueError, TypeError, AttributeError, binascii.Error):
        raise DavError(400) from None
    if len(content) > maximum:
        raise DavError(413)
    return content or b"\0", len(content)


def repeated_chunks(seed, size):
    # Make each block an integral number of seeds, preserving the byte sequence.
    block = seed * max(1, CHUNK // len(seed))
    while size:
        for offset in range(0, len(block), CHUNK):
            chunk = block[offset:offset + min(CHUNK, size)]
            yield chunk
            size -= len(chunk)
            if not size:
                return


class Store:
    def __init__(self, root, profile="strict"):
        if profile not in {"strict", "jianguoyun"}:
            raise ValueError("Unknown WebDAV fixture profile")
        self._profile = profile
        self.root = Path(root).resolve()
        self.root.mkdir()
        self.uploads = self.root.parent / "uploads"
        self.uploads.mkdir()
        self.lock = threading.RLock()
        self.stopping = threading.Event()
        self.initial_username = "webdav-test"
        self.initial_password = secrets.token_urlsafe(24)
        self.control_token = secrets.token_urlsafe(32)
        self.origin = ""
        self.epoch = 0
        self.serial = 0
        self.barriers = {}
        self.reset()

    @property
    def profile(self):
        return self._profile

    def reset(self):
        with self.lock:
            self.epoch += 1
            for barrier in self.barriers.values():
                barrier["release"].set()
            self.barriers = {}
            self.faults = []
            self.config = dict(DEFAULT_CONFIG)
            self.username = self.initial_username
            self.password = self.initial_password
            self.nonce = secrets.token_hex(24)
            self.previous_nonce = None
            self.opaque = secrets.token_hex(16)
            self.nonce_counts = {}
            if self.root.exists():
                shutil.rmtree(self.root)
            self.root.mkdir()
            self.entries = {}
            self.update("", "collection")
            self.requests = deque(maxlen=10000)
            self.request_sequence = 0
            self.method_counts = Counter()
            self.status_counts = Counter()
            self.internal_errors = 0

    def path(self, relative):
        relative_path(relative)
        result = self.root
        for component in relative.split("/") if relative else []:
            result = result / component
            try:
                info = result.lstat()
            except FileNotFoundError:
                continue
            if (stat.S_ISLNK(info.st_mode)
                    or getattr(info, "st_file_attributes", 0) & 0x400):
                raise DavError(403)
        if os.path.commonpath((str(self.root), str(result.resolve()))) != str(self.root):
            raise DavError(403)
        return result

    def update(self, path, kind, sha256=None, size=0):
        self.serial += 1
        tag = "%x-%s" % (self.serial, sha256 or "collection")
        metadata = {"path": path, "kind": kind, "size": size,
                    "etag": tag if self.profile == "jianguoyun" else '"' + tag + '"',
                    "modified_ms": int(time.time() * 1000)}
        if sha256 is not None:
            metadata["sha256"] = sha256
        self.entries[path] = metadata
        return dict(metadata)

    def parent(self, path, create=False):
        if not path:
            raise DavError(405)
        parts = path.split("/")[:-1]
        current = ""
        if "" not in self.entries:
            raise DavError(409)
        for part in parts:
            current = current + "/" + part if current else part
            entry = self.entries.get(current)
            if entry is None and create:
                self.check_capacity(current)
                self.path(current).mkdir()
                self.update(current, "collection")
            elif entry is None or entry["kind"] != "collection":
                raise DavError(409)

    def check_capacity(self, path):
        if path not in self.entries and len(self.entries) >= MAX_RESOURCES:
            raise DavError(507)

    def mkdir(self, path, parents=False):
        existing = self.entries.get(path)
        if existing:
            if existing["kind"] != "collection":
                raise DavError(409)
            return dict(existing)
        if path:
            self.parent(path, parents)
        self.check_capacity(path)
        self.path(path).mkdir()
        return self.update(path, "collection")

    def install(self, path, temporary, sha256, size, parents=False):
        self.parent(path, parents)
        existing = self.entries.get(path)
        if existing and existing["kind"] != "file":
            raise DavError(409)
        if existing is None and self.path(path).exists():
            # The host filesystem aliases two distinct DAV names (case/normalization).
            raise DavError(409)
        self.check_capacity(path)
        os.replace(temporary, self.path(path))
        return self.update(path, "file", sha256, size)

    def remove(self, path, recursive=False):
        entry = self.entries.get(path)
        if entry is None:
            raise DavError(404)
        target = self.path(path)
        if entry["kind"] == "collection":
            if recursive:
                shutil.rmtree(target)
            else:
                try:
                    target.rmdir()
                except OSError:
                    raise DavError(409) from None
            prefix = path + "/" if path else ""
            for child in list(self.entries):
                if child == path or child.startswith(prefix):
                    del self.entries[child]
        else:
            target.unlink()
            del self.entries[path]

    def configure(self, data):
        allowed = set(DEFAULT_CONFIG) | {"username", "password", "action"}
        if set(data) - allowed:
            raise DavError(400)
        proposed = dict(self.config)
        choices = {"auth": {"basic", "digest", "both"},
                   "namespace": {"prefix", "default"},
                   "href_mode": {"path", "absolute", "relative"},
                   "etag_mode": {"strong", "weak", "missing"}}
        for key in DEFAULT_CONFIG:
            if key not in data:
                continue
            value = data[key]
            if key in choices:
                if not isinstance(value, str) or value not in choices[key]:
                    raise DavError(400)
            elif key == "namespace_prefix":
                if (not isinstance(value, str) or len(value) > 32
                        or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", value)
                        or value.lower().startswith("xml")):
                    raise DavError(400)
            elif type(value) is not bool:
                raise DavError(400)
            proposed[key] = value
        for key in ("username", "password"):
            if key in data and (not isinstance(data[key], str) or not data[key]
                                or len(data[key]) > 1024 or "\x00" in data[key]):
                raise DavError(400)
        if ":" in data.get("username", self.username):
            raise DavError(400)
        self.config = proposed
        if "username" in data or "password" in data or "auth" in data:
            self.username = data.get("username", self.username)
            self.password = data.get("password", self.password)
            self.nonce = secrets.token_hex(24)
            self.previous_nonce = None
            self.nonce_counts.clear()

    def authorize(self, method, target, header):
        with self.lock:
            if (not header and self.config["anonymous_read"]
                    and method in {"GET", "PROPFIND", "OPTIONS"}):
                return True
            if not header:
                return False
            scheme, _, value = header.partition(" ")
            try:
                if scheme.lower() == "basic" and self.config["auth"] in {"basic", "both"}:
                    received = base64.b64decode(value, validate=True)
                    expected = (self.username + ":" + self.password).encode("utf-8")
                    return hmac.compare_digest(received, expected)
                if scheme.lower() != "digest" or self.config["auth"] not in {"digest", "both"}:
                    return False
                items = parse_http_list(value)
                parts = parse_keqv_list(items)
                if len(parts) != len(items):
                    return False
                if (parts.get("username") != self.username or parts.get("realm") != REALM
                        or parts.get("nonce") not in {self.nonce, self.previous_nonce}
                        or parts.get("uri") != target
                        or parts.get("opaque") != self.opaque or parts.get("qop") != "auth"
                        or parts.get("algorithm", "MD5").upper() != "MD5"
                        or not re.fullmatch(r"[0-9a-fA-F]{8}", parts.get("nc", ""))
                        or not 1 <= len(parts.get("cnonce", "")) <= 256):
                    return False
                key = parts["cnonce"]
                count = int(parts["nc"], 16)
                if parts["nonce"] == self.nonce and count <= self.nonce_counts.get(key, 0):
                    return False
                def md5(text):
                    return hashlib.md5(text.encode("utf-8")).hexdigest()
                ha1 = md5(self.username + ":" + REALM + ":" + self.password)
                ha2 = md5(method + ":" + target)
                expected = md5(":".join((ha1, parts["nonce"], parts["nc"], key, "auth", ha2)))
                if not hmac.compare_digest(expected, parts.get("response", "")):
                    return False
                if parts["nonce"] == self.previous_nonce:
                    raise DigestNonceExpired()
                if key not in self.nonce_counts and len(self.nonce_counts) >= 4096:
                    self.previous_nonce = self.nonce
                    self.nonce = secrets.token_hex(24)
                    self.nonce_counts.clear()
                    raise DigestNonceExpired()
                self.nonce_counts[key] = count
                return True
            except (ValueError, TypeError, UnicodeError, binascii.Error):
                return False

    def record(self, method, path):
        with self.lock:
            self.request_sequence += 1
            entry = {"sequence": self.request_sequence, "method": method,
                     "path": path, "status": None, "authenticated": False}
            self.requests.append(entry)
            self.method_counts[method] += 1
            return entry

    def rules(self, method, path, destination=None, depth=None):
        with self.lock:
            selected = []
            for rule in self.faults + list(self.barriers.values()):
                if (rule["count"] != 0 and rule["method"] in ("*", method)
                        and rule["path"] in ("*", path)
                        and (not rule.get("path_prefix") or path.startswith(rule["path_prefix"]))
                        and ("destination" not in rule or rule["destination"] == destination)
                        and ("depth" not in rule or rule["depth"] == depth)):
                    if rule.get("skip", 0):
                        rule["skip"] -= 1
                        continue
                    if rule["count"] > 0:
                        rule["count"] -= 1
                    selected.append(rule)
            self.faults = [rule for rule in self.faults if rule["count"] != 0]
            return selected

    def add_rule(self, data, barrier=False):
        method = data.get("method", "*")
        path = data.get("path", "*")
        phase = data.get("phase", "before")
        if method not in METHODS | {"*"} or phase not in {"before", "after", "after_headers"}:
            raise DavError(400)
        if path != "*":
            relative_path(path)
        count = integer(data.get("count", 1), -1, 1000000)
        if count == 0:
            raise DavError(400)
        rule = {"method": method, "path": path, "phase": phase, "count": count,
                "skip": integer(data.get("skip", 0), 0, 1000000)}
        if "path_prefix" in data:
            prefix = data["path_prefix"]
            if not isinstance(prefix, str) or not prefix:
                raise DavError(400)
            relative_path(prefix.rstrip("/"))
            rule["path_prefix"] = prefix
        if "depth" in data:
            if method != "PROPFIND" or data["depth"] not in {"0", "1"}:
                raise DavError(400)
            rule["depth"] = data["depth"]
        if "destination" in data:
            if method != "MOVE":
                raise DavError(400)
            rule["destination"] = relative_path(data["destination"])
        if barrier:
            name = data.get("id")
            if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", name)
                    or name in self.barriers or len(self.barriers) >= 256):
                raise DavError(400)
            drop_after_release = data.get("drop_after_release", False)
            if type(drop_after_release) is not bool or (drop_after_release and self.profile != "jianguoyun"):
                raise DavError(400)
            rule.update(effect="barrier", hits=0, reached=threading.Event(),
                        release=threading.Event(), drop_after_release=drop_after_release)
            self.barriers[name] = rule
            return
        if len(self.faults) >= 256:
            raise DavError(400)
        effect = data.get("effect")
        effects = {"status", "redirect", "xml", "delay", "drop", "truncate",
                   "throttle", "weak_etag", "missing_etag"}
        if self.profile == "jianguoyun":
            effects.update({"raw_etag", "quoted_etag"})
        if effect not in effects:
            raise DavError(400)
        if phase == "after_headers" and effect not in {"delay", "drop"}:
            raise DavError(400)
        headers = data.get("headers", {})
        if not isinstance(headers, dict) or len(headers) > 32:
            raise DavError(400)
        header_size = 0
        for name, value in headers.items():
            if (not re.fullmatch(r"[A-Za-z0-9-]{1,128}", name)
                    or name.lower() in {"content-length", "transfer-encoding", "connection"}
                    or not isinstance(value, str)
                    or any(ord(c) < 32 or ord(c) > 126 for c in value)):
                raise DavError(400)
            header_size += len(name) + len(value)
        if header_size > 16384:
            raise DavError(400)
        rule["headers"] = dict(headers)
        rule["effect"] = effect
        if effect == "status":
            rule["status"] = integer(data.get("status"), 400, 599)
        elif effect == "redirect":
            location = data.get("location")
            if (not isinstance(location, str) or len(location) > 8192
                    or any(ord(c) < 32 or ord(c) > 126 for c in location)):
                raise DavError(400)
            rule.update(location=location, status=integer(data.get("status", 301), 300, 399))
        elif effect == "xml":
            if "resource_count" in data:
                if any(key in data for key in ("body", "text", "content_base64", "size")):
                    raise DavError(400)
                rule["resource_count"] = integer(data["resource_count"], 1, MAX_RESOURCES)
            elif "body" in data:
                rule["seed"], rule["size"] = bytes_spec({"text": data["body"]}, MAX_CONTROL)
            else:
                rule["seed"], rule["size"] = bytes_spec(data, MAX_XML)
        elif effect in {"delay", "throttle"}:
            rule["delay_ms"] = integer(data.get("delay_ms"), 0, 1800000)
        elif effect == "truncate":
            rule["bytes"] = integer(data.get("bytes"), 0, MAX_FILE)
        self.faults.append(rule)

    def control(self, data):
        action = data.get("action")
        if action == "wait":
            with self.lock:
                barrier = self.barriers.get(data.get("id"))
            if barrier is None:
                raise DavError(404)
            timeout = integer(data.get("timeout_ms", 5000), 0, 60000) / 1000
            reached = barrier["reached"].wait(timeout)
            return {"reached": reached, "hits": barrier["hits"]}
        with self.lock:
            if action == "reset":
                self.reset()
            elif action == "configure":
                self.configure(data)
            elif action in {"fault", "barrier"}:
                self.add_rule(data, action == "barrier")
            elif action == "release":
                barrier = self.barriers.get(data.get("id"))
                if barrier is None:
                    raise DavError(404)
                barrier["release"].set()
            elif action == "clear_faults":
                self.faults.clear()
                for barrier in self.barriers.values():
                    barrier["release"].set()
                self.barriers.clear()
            elif action == "requests":
                since = integer(data.get("since", 0), 0, 2**63 - 1)
                limit = integer(data.get("limit", 1000), 1, 10000)
                entries = [dict(item) for item in self.requests if item["sequence"] > since][:limit]
                return {"requests": entries, "total": self.request_sequence,
                        "oldest": self.requests[0]["sequence"] if self.requests else 0,
                        "methods": dict(self.method_counts), "statuses": dict(self.status_counts),
                        "internal_errors": self.internal_errors}
            elif action == "tree":
                offset = integer(data.get("offset", 0), 0, MAX_RESOURCES)
                limit = integer(data.get("limit", 1000), 1, 1000)
                names = sorted(self.entries)
                return {"entries": [dict(self.entries[p]) for p in names[offset:offset + limit]],
                        "total": len(names)}
            elif action in {"put", "mkdir", "delete", "inspect", "touch_etag"}:
                path = relative_path(data.get("path"))
                if action == "mkdir":
                    return self.mkdir(path, data.get("parents", False))
                if action == "delete":
                    self.remove(path, data.get("recursive", False))
                elif action == "inspect":
                    entry = self.entries.get(path)
                    if entry is None:
                        return {"exists": False}
                    result = dict(entry, exists=True)
                    if data.get("content") and entry["kind"] == "file":
                        if entry["size"] > MAX_CONTROL:
                            raise DavError(413)
                        result["content_base64"] = base64.b64encode(self.path(path).read_bytes()).decode("ascii")
                    return result
                elif action == "touch_etag":
                    entry = self.entries.get(path)
                    if entry is None:
                        raise DavError(404)
                    return self.update(path, entry["kind"], entry.get("sha256"), entry["size"])
                elif action == "put":
                    seed, size = bytes_spec(data, MAX_FILE)
                    temporary = None
                    try:
                        with tempfile.NamedTemporaryFile(dir=self.uploads, delete=False) as stream:
                            temporary = stream.name
                            digest = hashlib.sha256()
                            for chunk in repeated_chunks(seed, size):
                                if self.stopping.is_set():
                                    raise DavError(503)
                                stream.write(chunk)
                                digest.update(chunk)
                        return self.install(path, temporary, digest.hexdigest(), size,
                                            data.get("parents", False))
                    finally:
                        if temporary and os.path.exists(temporary):
                            os.unlink(temporary)
            else:
                raise DavError(400)
        return {}


class DavHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "WebDavFixture"
    sys_version = ""

    def setup(self):
        self.request.settimeout(30)
        if isinstance(self.request, ssl.SSLSocket):
            self.request.do_handshake()
        super().setup()

    def log_message(self, *_args):
        pass

    def send_error(self, code, message=None, explain=None):
        # BaseHTTPRequestHandler otherwise reflects request details into HTML.
        self.close_connection = True
        self.reply(code)

    @property
    def store(self):
        return self.server.store

    def handle_expect_100(self):
        # Authenticate in dispatch first, before inviting a potentially huge body.
        self.expect_continue = True
        return True

    def body_chunks(self, maximum):
        transfer = self.headers.get("Transfer-Encoding")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) > 1 or (transfer and lengths):
            raise DavError(400)
        if transfer and transfer.lower() != "chunked":
            raise DavError(400)
        if getattr(self, "expect_continue", False):
            self.send_response_only(100)
            self.end_headers()
            self.expect_continue = False
        total = 0
        if transfer:
            while True:
                line = self.rfile.readline(8193)
                if len(line) > 8192 or not line.endswith(b"\r\n"):
                    raise DavError(400)
                size_text = line[:-2].split(b";", 1)[0]
                if not re.fullmatch(b"[0-9A-Fa-f]{1,16}", size_text):
                    raise DavError(400)
                size = int(size_text, 16)
                total += size
                if total > maximum:
                    raise DavError(413)
                if size == 0:
                    trailer_size = 0
                    while True:
                        line = self.rfile.readline(8193)
                        trailer_size += len(line)
                        if (len(line) > 8192 or trailer_size > CHUNK
                                or not line.endswith(b"\r\n")):
                            raise DavError(400)
                        if line == b"\r\n":
                            return
                yield from self.read_exact_chunks(size)
                if self.rfile.read(2) != b"\r\n":
                    raise DavError(400)
        else:
            text = lengths[0] if lengths else "0"
            if not re.fullmatch(r"[0-9]{1,20}", text):
                raise DavError(400)
            length = int(text)
            if length > maximum:
                raise DavError(413)
            yield from self.read_exact_chunks(length)

    def read_exact_chunks(self, length):
        while length:
            if self.store.stopping.is_set():
                raise DavError(503)
            data = self.rfile.read(min(CHUNK, length))
            if not data:
                raise DavError(400)
            length -= len(data)
            yield data

    def reply(self, status, payload=b"", headers=None, stream=None, size=None):
        if getattr(self, "record", None) is not None:
            with self.store.lock:
                self.record["status"] = status
                self.store.status_counts[str(status)] += 1
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload) if size is None else size))
        self.send_header("Connection", "close")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.close_connection = True
        self.response_started = True
        if 200 <= status < 300 and self.phases("after_headers"):
            return
        if stream is None:
            self.wfile.write(payload)
            return
        remaining = size
        limit = getattr(self, "truncate", None)
        if limit is not None:
            remaining = min(remaining, limit)
        while remaining:
            if self.store.stopping.is_set():
                return
            delay = getattr(self, "throttle_ms", 0)
            if delay and self.store.stopping.wait(delay / 1000):
                return
            chunk = stream.read(min(CHUNK, remaining))
            if not chunk:
                raise OSError("fixture stream ended early")
            self.wfile.write(chunk)
            self.wfile.flush()
            remaining -= len(chunk)
        if limit is not None and limit < size:
            self.drop()

    def drop(self):
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.connection.close()

    def fail(self, status):
        if getattr(self, "response_started", False):
            self.drop()
        else:
            self.reply(status)

    def challenge(self, stale=False):
        with self.store.lock:
            mode = self.store.config["auth"]
            nonce, opaque = self.store.nonce, self.store.opaque
        with self.store.lock:
            self.record["status"] = 401
            self.store.status_counts["401"] += 1
        self.send_response(401)
        if mode in {"digest", "both"}:
            challenge = ('Digest realm="%s", nonce="%s", '
                         'opaque="%s", algorithm=MD5, qop="auth"' % (REALM, nonce, opaque))
            self.send_header("WWW-Authenticate", challenge + (", stale=true" if stale else ""))
        if mode in {"basic", "both"}:
            self.send_header("WWW-Authenticate", 'Basic realm="%s", charset="UTF-8"' % REALM)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def phases(self, phase):
        for rule in self.active_rules:
            if rule["phase"] != phase:
                continue
            effect = rule["effect"]
            if effect == "barrier":
                with self.store.lock:
                    rule["hits"] += 1
                    rule["reached"].set()
                while not rule["release"].wait(0.05):
                    if self.store.stopping.is_set():
                        raise DavError(503)
                if rule.get("drop_after_release"):
                    self.drop()
                    return True
            elif effect == "delay":
                if self.store.stopping.wait(rule["delay_ms"] / 1000):
                    raise DavError(503)
            elif effect == "drop":
                self.drop()
                return True
            elif effect in {"status", "redirect"}:
                headers = dict(rule["headers"])
                if effect == "redirect":
                    headers["Location"] = rule["location"]
                self.reply(rule["status"], headers=headers)
                return True
            elif effect == "xml":
                with tempfile.TemporaryFile(dir=self.store.uploads) as stream:
                    chunks = (self.synthetic_listing(rule["resource_count"])
                              if "resource_count" in rule else
                              repeated_chunks(rule["seed"], rule["size"]))
                    for chunk in chunks:
                        if self.store.stopping.is_set():
                            raise DavError(503)
                        stream.write(chunk)
                        if stream.tell() > MAX_XML:
                            raise DavError(507)
                    size = stream.tell()
                    stream.seek(0)
                    headers = {"Content-Type": "application/xml; charset=utf-8"}
                    headers.update(rule["headers"])
                    self.reply(207, headers=headers, stream=stream, size=size)
                return True
        return False

    def synthetic_listing(self, count):
        path = self.record["path"]
        with self.store.lock:
            entry = self.store.entries.get(path)
            if entry is None:
                raise DavError(404)
            if entry["kind"] != "collection":
                raise DavError(409)
            etag = entry["etag"]
        base = "/notebook/" + "/".join(quote(p, safe="") for p in path.split("/"))
        if not base.endswith("/"):
            base += "/"
        def response(href, resource_type, tag):
            return ('<response><href>%s</href><propstat><prop>%s<getetag>%s</getetag>'
                    '<getcontentlength>0</getcontentlength></prop>'
                    '<status>HTTP/1.1 200 OK</status></propstat></response>' %
                    (escape(href), resource_type, escape(tag))).encode("utf-8")
        yield b'<multistatus xmlns="DAV:">'
        yield response(base, "<resourcetype><collection/></resourcetype>", etag)
        for index in range(count):
            yield response(base + "r%d" % index, "<resourcetype/>", '"r%d"' % index)
        yield b"</multistatus>"

    def presented_etag(self, metadata):
        mode = self.store.config["etag_mode"]
        for rule in self.active_rules:
            if rule["effect"] == "weak_etag":
                mode = "weak"
            elif rule["effect"] == "missing_etag":
                mode = "missing"
            elif rule["effect"] == "raw_etag":
                mode = "raw"
            elif rule["effect"] == "quoted_etag":
                mode = "quoted"
        if mode == "missing":
            return None
        tag = metadata["etag"]
        if mode == "raw":
            return tag.strip('"')
        if mode in {"weak", "quoted"} and not tag.startswith('"'):
            tag = '"' + tag + '"'
        return ("W/" if mode == "weak" else "") + tag

    def check_epoch(self):
        if self.epoch != self.store.epoch:
            raise DavError(409)

    def expected(self, entry, required=False, ignore=False):
        if ignore:
            return
        match = self.headers.get("If-Match")
        absent = self.headers.get("If-None-Match")
        if match and absent:
            raise DavError(400)
        if absent is not None:
            if absent != "*":
                raise DavError(400)
            if entry is not None:
                raise DavError(412)
        elif match is not None:
            if entry is None or match != entry["etag"]:
                raise DavError(412)
        elif required:
            raise DavError(428)

    def href(self, metadata, queried):
        path = "/notebook/" + "/".join(quote(p, safe="") for p in metadata["path"].split("/"))
        if metadata["kind"] == "collection" and not path.endswith("/"):
            path += "/"
        mode = self.store.config["href_mode"]
        if mode == "absolute":
            return self.store.origin + path
        if mode == "relative":
            requested = "/notebook" + ("/" + "/".join(quote(p, safe="") for p in queried.split("/"))
                                       if queried else "")
            if urlsplit(self.path).path.endswith("/") and not requested.endswith("/"):
                requested += "/"
            base = requested.rsplit("/", 1)[0] + "/"
            relative = posixpath.relpath(path, base)
            return relative + "/" if path.endswith("/") else relative
        return path

    def propfind(self, path):
        depth = self.headers.get("Depth")
        if depth not in {"0", "1"}:
            raise DavError(403)
        entry = self.store.entries.get(path)
        if entry is None:
            raise DavError(404)
        prefix = self.store.config["namespace_prefix"]
        prefixed = self.store.config["namespace"] == "prefix"
        tag = (lambda name: prefix + ":" + name) if prefixed else (lambda name: name)
        namespace = 'xmlns:%s="DAV:"' % prefix if prefixed else 'xmlns="DAV:"'
        stream = tempfile.TemporaryFile(dir=self.store.uploads)
        try:
            stream.write(('<?xml version="1.0" encoding="utf-8"?><%s %s>' %
                          (tag("multistatus"), namespace)).encode("utf-8"))
            def append(metadata):
                etag = self.presented_etag(metadata)
                properties = "<%s>%s</%s>" % (tag("resourcetype"),
                              "<%s/>" % tag("collection") if metadata["kind"] == "collection" else "",
                              tag("resourcetype"))
                if etag is not None:
                    properties += "<%s>%s</%s>" % (tag("getetag"), escape(etag), tag("getetag"))
                properties += "<%s>%d</%s>" % (tag("getcontentlength"), metadata["size"], tag("getcontentlength"))
                properties += "<%s>%s</%s>" % (
                    tag("getlastmodified"), formatdate(metadata["modified_ms"] / 1000, usegmt=True),
                    tag("getlastmodified"))
                text = ("<{response}><{href}>{url}</{href}><{propstat}><{prop}>{properties}"
                        "</{prop}><{status}>HTTP/1.1 200 OK</{status}></{propstat}></{response}>")
                stream.write(text.format(response=tag("response"), href=tag("href"),
                                         url=escape(self.href(metadata, path)), propstat=tag("propstat"),
                                         prop=tag("prop"), properties=properties,
                                         status=tag("status")).encode("utf-8"))
                if stream.tell() > MAX_XML:
                    raise DavError(507)
            append(entry)
            if depth == "1" and entry["kind"] == "collection":
                prefix_path = path + "/" if path else ""
                returned = 1
                for name, child in self.store.entries.items():
                    if name != path and name.startswith(prefix_path) and "/" not in name[len(prefix_path):]:
                        if self.store.profile == "jianguoyun" and returned >= 750:
                            break
                        append(child)
                        returned += 1
            stream.write(("</%s>" % tag("multistatus")).encode("utf-8"))
            size = stream.tell()
            stream.seek(0)
            return 207, {"Content-Type": "application/xml; charset=utf-8"}, stream, size
        except BaseException:
            stream.close()
            raise

    def operation(self, path):
        method = self.command
        if method == "PUT":
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=self.store.uploads, delete=False) as stream:
                    temporary = stream.name
                    digest, size = hashlib.sha256(), 0
                    for chunk in self.body_chunks(MAX_FILE):
                        stream.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                with self.store.lock:
                    self.check_epoch()
                    previous = self.store.entries.get(path)
                    if self.store.profile == "jianguoyun":
                        match = self.headers.get("If-Match")
                        if match is not None and (previous is None or match != previous["etag"]):
                            raise DavError(412)
                    else:
                        self.expected(previous, required=True,
                                      ignore=self.store.config["ignore_create_conditions"])
                    metadata = self.store.install(path, temporary, digest.hexdigest(), size)
                    etag = None if self.store.profile == "jianguoyun" else self.presented_etag(metadata)
                    return 204 if previous else 201, {"ETag": etag} if etag else {}, None, 0
            finally:
                if temporary and os.path.exists(temporary):
                    os.unlink(temporary)
        body_size = sum(len(chunk) for chunk in self.body_chunks(CHUNK))
        with self.store.lock:
            self.check_epoch()
            entry = self.store.entries.get(path)
            if method == "OPTIONS":
                return 200, {"DAV": "1", "Allow": ", ".join(sorted(METHODS))}, None, 0
            if method == "PROPFIND":
                return self.propfind(path)
            if method == "GET":
                if entry is None:
                    raise DavError(404)
                if entry["kind"] != "file":
                    raise DavError(405)
                self.expected(entry, ignore=self.store.profile == "jianguoyun")
                # Snapshot the inspected representation before releasing the lock. In
                # particular, Windows forbids replacing an ordinary open file handle.
                stream = tempfile.TemporaryFile(dir=self.store.uploads)
                try:
                    with self.store.path(path).open("rb") as source:
                        shutil.copyfileobj(source, stream, CHUNK)
                    stream.seek(0)
                except BaseException:
                    stream.close()
                    raise
                etag = self.presented_etag(entry)
                headers = {"Content-Type": "application/octet-stream"}
                if etag:
                    headers["ETag"] = etag
                return 200, headers, stream, entry["size"]
            if method == "MKCOL":
                if body_size:
                    raise DavError(415)
                if entry is not None:
                    if self.store.profile != "jianguoyun":
                        raise DavError(405)
                    if entry["kind"] != "collection":
                        raise DavError(409)
                    return 201, {}, None, 0
                self.expected(entry, ignore=self.store.profile == "jianguoyun")
                self.store.mkdir(path)
                return 201, {}, None, 0
            if method == "DELETE":
                if entry is None:
                    raise DavError(404)
                if entry["kind"] != "file":
                    raise DavError(405)
                self.expected(entry, required=True,
                              ignore=(self.store.profile == "jianguoyun" or
                                      self.store.config["ignore_delete_conditions"]))
                self.store.remove(path)
                return 204, {}, None, 0
            if method == "MOVE":
                if entry is None:
                    raise DavError(404)
                if entry["kind"] != "file":
                    raise DavError(405)
                self.expected(entry, ignore=self.store.profile == "jianguoyun")
                destination = self.headers.get("Destination", "")
                parsed = urlsplit(destination)
                if (parsed.scheme + "://" + parsed.netloc != self.store.origin
                        or parsed.query or parsed.fragment or parsed.username):
                    raise DavError(400)
                target = decode_url_path(parsed.path)
                if target == path:
                    raise DavError(403)
                old = self.store.entries.get(target)
                overwrite = self.headers.get("Overwrite")
                if overwrite not in {"T", "F"}:
                    raise DavError(400)
                if self.store.profile == "jianguoyun":
                    if old is not None:
                        raise DavError(409)
                elif not self.store.config["ignore_move_conditions"]:
                    if overwrite == "F":
                        if old is not None:
                            raise DavError(412)
                    else:
                        tagged = re.fullmatch(r'<([^<>]+)>\s+\(\[("[^"\r\n]+")\]\)',
                                              self.headers.get("If", ""))
                        if tagged is None:
                            raise DavError(428)
                        if tagged[1] != destination or old is None or tagged[2] != old["etag"]:
                            raise DavError(412)
                if old is not None and old["kind"] != "file":
                    raise DavError(409)
                if old is None and self.store.path(target).exists():
                    raise DavError(409)
                self.store.parent(target)
                self.store.check_capacity(target)
                os.replace(self.store.path(path), self.store.path(target))
                del self.store.entries[path]
                metadata = self.store.update(target, "file", entry["sha256"], entry["size"])
                etag = self.presented_etag(metadata)
                return 204 if old else 201, {"ETag": etag} if etag else {}, None, 0
        raise DavError(405)

    def dispatch(self):
        self.record = None
        self.active_rules = []
        self.response_started = False
        stream = None
        try:
            parsed = urlsplit(self.path)
            if parsed.scheme or parsed.netloc or parsed.query or parsed.fragment:
                raise DavError(400)
            if parsed.path == "/__control":
                if self.command != "POST":
                    raise DavError(405)
                expected = "Bearer " + self.store.control_token
                received = self.headers.get("Authorization", "")
                if not hmac.compare_digest(received.encode("utf-8"), expected.encode("ascii")):
                    raise DavError(403)
                if self.headers.get_content_type() != "application/json":
                    raise DavError(415)
                data = json.loads(b"".join(self.body_chunks(MAX_CONTROL)).decode("utf-8"))
                if not isinstance(data, dict):
                    raise DavError(400)
                response = self.store.control(data)
                payload = json.dumps(dict(response, ok=True), ensure_ascii=True).encode("utf-8")
                self.reply(200, payload, {"Content-Type": "application/json"})
                return
            path = decode_url_path(parsed.path)
            self.record = self.store.record(self.command, path)
            if self.store.profile == "jianguoyun" and self.command == "PROPFIND":
                depth = self.headers.get("Depth")
                with self.store.lock:
                    self.record["depth"] = depth if depth in {"0", "1"} else "invalid"
            if self.command not in METHODS:
                raise DavError(405)
            try:
                authorized = self.store.authorize(self.command, self.path, self.headers.get("Authorization"))
            except DigestNonceExpired:
                self.challenge(stale=True)
                return
            if not authorized:
                self.challenge()
                return
            with self.store.lock:
                self.record["authenticated"] = bool(self.headers.get("Authorization"))
                self.epoch = self.store.epoch
                destination = None
                if self.command == "MOVE":
                    try:
                        destination = decode_url_path(urlsplit(self.headers.get("Destination", "")).path)
                    except DavError:
                        pass
                if self.store.profile == "jianguoyun":
                    if destination is not None:
                        self.record["destination"] = destination
                self.active_rules = self.store.rules(self.command, path, destination,
                                                     self.headers.get("Depth"))
            for rule in self.active_rules:
                if rule["effect"] == "truncate":
                    self.truncate = rule["bytes"]
                elif rule["effect"] == "throttle":
                    self.throttle_ms = rule["delay_ms"]
            if self.phases("before"):
                return
            status, headers, stream, size = self.operation(path)
            if self.phases("after"):
                return
            self.reply(status, headers=headers, stream=stream, size=size)
        except DavError as error:
            self.fail(error.status)
        except (ValueError, TypeError, UnicodeError, KeyError):
            self.fail(400)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.timeout):
            self.close_connection = True
        except OSError:
            self.fail(507)
        except Exception:
            with self.store.lock:
                self.store.internal_errors += 1
            self.fail(500)
        finally:
            if stream is not None:
                stream.close()

    do_OPTIONS = dispatch
    do_PROPFIND = dispatch
    do_GET = dispatch
    do_PUT = dispatch
    do_MKCOL = dispatch
    do_MOVE = dispatch
    do_DELETE = dispatch
    do_POST = dispatch
    do_HEAD = dispatch


class LoopbackServer(ThreadingHTTPServer):
    allow_reuse_address = False
    daemon_threads = False
    block_on_close = True

    def __init__(self, store, tls_context):
        self.store = store
        self.tls_context = tls_context
        self.connections = set()
        self.connection_lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(("127.0.0.1", 0), DavHandler)

    def verify_request(self, request, address):
        return ipaddress.ip_address(address[0]).is_loopback

    def get_request(self):
        connection, address = super().get_request()
        if self.tls_context:
            connection = self.tls_context.wrap_socket(connection, server_side=True,
                                                      do_handshake_on_connect=False)
        return connection, address

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        with self.connection_lock:
            self.connections.add(request)
        try:
            super().process_request(request, address)
        except BaseException:
            with self.connection_lock:
                self.connections.discard(request)
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            with self.connection_lock:
                self.connections.discard(request)
            self.slots.release()

    def handle_error(self, request, client_address):
        # Failed handshakes/disconnects are expected test inputs; no tracebacks.
        pass

    def close_connections(self):
        with self.connection_lock:
            connections = list(self.connections)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()


class WebDavFixture:
    """Own the listener, not the supplied temporary directory or child processes."""
    def __init__(self, directory, tls_context=None, profile="strict"):
        self.store = Store(Path(directory) / "remote", profile)
        self.server = LoopbackServer(self.store, tls_context)
        scheme = "https" if tls_context else "http"
        self.store.origin = "%s://127.0.0.1:%d" % (scheme, self.server.server_port)
        self.url = self.store.origin + "/notebook/"
        self.control_url = self.store.origin + "/__control"
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.05}, name="webdav-fixture")
        self.thread.start()

    def environment(self):
        return {"VXCORE_WEBDAV_TEST_URL": self.url,
                "VXCORE_WEBDAV_TEST_PROFILE": self.store.profile,
                "VXCORE_WEBDAV_TEST_USERNAME": self.store.initial_username,
                "VXCORE_WEBDAV_TEST_PASSWORD": self.store.initial_password,
                "VXCORE_WEBDAV_TEST_CONTROL_URL": self.control_url,
                "VXCORE_WEBDAV_TEST_CONTROL_TOKEN": self.store.control_token}

    def close(self):
        self.store.stopping.set()
        with self.store.lock:
            for barrier in self.store.barriers.values():
                barrier["release"].set()
        self.server.shutdown()
        self.server.close_connections()
        self.server.server_close()
        self.thread.join()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()
