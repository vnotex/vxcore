"""Run a child against an owned hermetic WebDAV fixture.

  python libs/vxcore/tests/run_webdav_test.py -- executable [arguments ...]
  python libs/vxcore/tests/run_webdav_test.py --http -- executable [arguments ...]
  python libs/vxcore/tests/run_webdav_test.py --profile jianguoyun -- executable [arguments ...]

TLS is the default and requires Python's ssl module plus OpenSSL >= 1.1.1. Resolve
OpenSSL from OPENSSL (explicit executable), PATH, or the usr/bin directory of a
Git-for-Windows installation discovered on PATH. Generate two-day self-signed
certificates with SAN DNS:localhost,IP:127.0.0.1 in an owned temporary directory;
never install anything in a system trust store. --http is for direct core tests.
A missing/broken TLS prerequisite fails the run, never falls back to plain HTTP.

Child environment:
  VXCORE_WEBDAV_TEST_URL              dedicated /notebook/ collection
  VXCORE_WEBDAV_TEST_PROFILE          immutable strict(default) or jianguoyun semantics
  VXCORE_WEBDAV_TEST_CA_FILE          explicit TLS trust bundle (TLS mode only)
  VXCORE_WEBDAV_TEST_UNTRUSTED_CA_FILE unrelated certificate for negative tests
  VXCORE_WEBDAV_TEST_USERNAME / VXCORE_WEBDAV_TEST_PASSWORD
  VXCORE_WEBDAV_TEST_CONTROL_URL / VXCORE_WEBDAV_TEST_CONTROL_TOKEN
  VXCORE_WEBDAV_TEST_CLIENT_ROOT      unique client-owned workspace directory
  VXCORE_WEBDAV_TEST_RUNNER / VXCORE_WEBDAV_TEST_PYTHON  for launching another client
  TMP / TEMP / TMPDIR                 unique client temp, set before child startup

The executable still MUST enable vxcore test mode before creating any context.
For two-device/crash tests an orchestrating child can launch this SAME runner with
--reuse-fixture -- executable ... and the inherited fixture environment. It keeps
that live endpoint, selected profile and remote content, allocates a fresh owned client root/temp,
and does not stop the borrowed fixture. Each invocation is a separate process and
independent local baseline. A crash-recovery orchestrator may keep its baseline
in its own CLIENT_ROOT and pass that path explicitly to successive child clients;
neither nested runner will delete its caller's workspace. The owning runner must
remain alive until all clients finish. Python orchestrators can also import
isolated_client_environment() and WebDavFixture directly.

The child inherits the terminal; commands, secrets, headers and payloads are not
printed. Normal child exit status is propagated; POSIX signal exits map to the
shell-compatible 128+signal convention. Ctrl-C terminates/waits for this runner's
child. Only the owned listener, direct child and unique temporary directory are
cleaned up. The complete authenticated control protocol is in webdav_server.py.
"""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, HTTPSHandler, build_opener

sys.dont_write_bytecode = True

from webdav_server import WebDavFixture


class FixtureError(Exception):
    pass


def openssl_executable():
    explicit = os.environ.get("OPENSSL")
    candidates = [explicit] if explicit else [shutil.which("openssl")]
    git = shutil.which("git")
    if not explicit and git:
        git_path = Path(git).resolve()
        for parent in git_path.parents:
            candidate = parent / "usr" / "bin" / "openssl.exe"
            if candidate.is_file():
                candidates.append(str(candidate))
                break
    for candidate in candidates:
        if not candidate:
            continue
        try:
            result = subprocess.run([candidate, "version"], capture_output=True,
                                    timeout=15, check=False)
        except (OSError, subprocess.TimeoutExpired):
            continue
        match = re.match(rb"OpenSSL (\d+)\.(\d+)\.(\d+)", result.stdout)
        if result.returncode == 0 and match and tuple(map(int, match.groups())) >= (1, 1, 1):
            return candidate
    raise FixtureError("TLS fixture requires an OpenSSL >= 1.1.1 executable (OPENSSL or PATH)")


def make_certificate(executable, directory, name):
    certificate = directory / (name + ".crt")
    key = directory / (name + ".key")
    arguments = [executable, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-sha256", "-days", "2", "-subj", "/CN=localhost",
                 "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                 "-addext", "basicConstraints=critical,CA:TRUE",
                 "-addext", "extendedKeyUsage=serverAuth",
                 "-keyout", str(key), "-out", str(certificate)]
    try:
        subprocess.run(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=60, check=True)
    except (OSError, subprocess.SubprocessError):
        raise FixtureError("OpenSSL could not generate the temporary TLS certificate") from None
    key.chmod(0o600)
    return certificate, key


def isolated_client_environment(directory, base_environment):
    """Allocate one fresh client's workspace/temp under an already owned directory."""
    root = Path(tempfile.mkdtemp(prefix="client-", dir=directory))
    temporary = root / "tmp"
    temporary.mkdir()
    environment = dict(base_environment)
    environment.update({"TMP": str(temporary), "TEMP": str(temporary), "TMPDIR": str(temporary),
                        "VXCORE_WEBDAV_TEST_CLIENT_ROOT": str(root),
                        "VXCORE_WEBDAV_TEST_RUNNER": str(Path(__file__).resolve()),
                        "VXCORE_WEBDAV_TEST_PYTHON": sys.executable})
    return environment


def validate_borrowed_environment(environment):
    if environment.get("VXCORE_WEBDAV_TEST_PROFILE", "strict") not in {"strict", "jianguoyun"}:
        raise FixtureError("--reuse-fixture has an invalid inherited profile")
    required = ("VXCORE_WEBDAV_TEST_URL", "VXCORE_WEBDAV_TEST_USERNAME",
                "VXCORE_WEBDAV_TEST_PASSWORD", "VXCORE_WEBDAV_TEST_CONTROL_URL",
                "VXCORE_WEBDAV_TEST_CONTROL_TOKEN")
    if any(not environment.get(name) for name in required):
        raise FixtureError("--reuse-fixture requires the inherited fixture environment")
    origins = []
    for name, expected_path in ((required[0], "/notebook/"), (required[3], "/__control")):
        try:
            parsed = urlsplit(environment[name])
            loopback = parsed.hostname == "localhost" or ipaddress.ip_address(parsed.hostname).is_loopback
            if (not loopback or parsed.scheme not in {"http", "https"} or parsed.username
                    or parsed.password or parsed.query or parsed.fragment or parsed.path != expected_path
                    or parsed.port is None):
                raise ValueError
            origins.append((parsed.scheme, parsed.hostname, parsed.port))
        except (ValueError, TypeError):
            raise FixtureError("--reuse-fixture requires matching loopback fixture URLs") from None
    if origins[0] != origins[1]:
        raise FixtureError("--reuse-fixture URLs must have the same origin")
    if origins[0][0] == "https":
        bundle = environment.get("VXCORE_WEBDAV_TEST_CA_FILE")
        if not bundle or not Path(bundle).is_file():
            raise FixtureError("--reuse-fixture TLS endpoint requires its explicit test CA file")


def check_ready(environment):
    """Exercise the authenticated listener before any child/context is created."""
    handlers = [ProxyHandler({})]
    if environment["VXCORE_WEBDAV_TEST_CONTROL_URL"].startswith("https:"):
        context = ssl.create_default_context(cafile=environment["VXCORE_WEBDAV_TEST_CA_FILE"])
        handlers.append(HTTPSHandler(context=context))
    opener = build_opener(*handlers)
    request = Request(environment["VXCORE_WEBDAV_TEST_CONTROL_URL"],
                      data=b'{"action":"requests","limit":1}', method="POST",
                      headers={"Content-Type": "application/json",
                               "Authorization": "Bearer " + environment["VXCORE_WEBDAV_TEST_CONTROL_TOKEN"]})
    try:
        with opener.open(request, timeout=10) as response:
            body = response.read(65537)
            if response.status != 200 or len(body) > 65536 or not json.loads(body).get("ok"):
                raise FixtureError("WebDAV fixture readiness check failed")
    except (OSError, ValueError):
        raise FixtureError("WebDAV fixture authenticated readiness check failed") from None


def run_child(command, environment):
    child = None
    try:
        child = subprocess.Popen(command, env=environment)
        status = child.wait()
        return status if status >= 0 else 128 - status
    except KeyboardInterrupt:
        return 130
    except OSError:
        raise FixtureError("Could not launch the requested test executable") from None
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--http", action="store_true", help="use explicit loopback HTTP instead of TLS")
    parser.add_argument("--reuse-fixture", action="store_true", help="borrow the inherited running fixture")
    parser.add_argument("--profile", choices=("strict", "jianguoyun"), default=None,
                        help="immutable provider semantics (default: strict; inherited on reuse)")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args(argv)
    command = options.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("expected -- executable [arguments ...]")
    if options.reuse_fixture and options.http:
        parser.error("--reuse-fixture inherits its protocol; do not specify --http")
    try:
        with tempfile.TemporaryDirectory(prefix="vxcore-webdav-") as temporary:
            directory = Path(temporary)
            directory.chmod(0o700)
            environment = dict(os.environ)
            if options.reuse_fixture:
                validate_borrowed_environment(environment)
                inherited = environment.get("VXCORE_WEBDAV_TEST_PROFILE", "strict")
                if options.profile is not None and options.profile != inherited:
                    raise FixtureError("--reuse-fixture cannot change its inherited profile")
                environment["VXCORE_WEBDAV_TEST_PROFILE"] = inherited
                check_ready(environment)
                return run_child(command, isolated_client_environment(directory, environment))
            environment.pop("VXCORE_WEBDAV_TEST_CA_FILE", None)
            environment.pop("VXCORE_WEBDAV_TEST_UNTRUSTED_CA_FILE", None)
            tls_context = None
            if not options.http:
                executable = openssl_executable()
                tls_directory = directory / "tls"
                tls_directory.mkdir()
                certificate, key = make_certificate(executable, tls_directory, "localhost")
                unrelated, _ = make_certificate(executable, tls_directory, "unrelated")
                tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
                tls_context.load_cert_chain(certificate, key)
                environment["VXCORE_WEBDAV_TEST_CA_FILE"] = str(certificate)
                environment["VXCORE_WEBDAV_TEST_UNTRUSTED_CA_FILE"] = str(unrelated)
            with WebDavFixture(directory, tls_context, options.profile or "strict") as fixture:
                environment.update(fixture.environment())
                check_ready(environment)
                status = run_child(command, isolated_client_environment(directory, environment))
                if not status and fixture.store.internal_errors:
                    raise FixtureError("WebDAV fixture encountered an internal request failure")
                return status
    except FixtureError as error:
        print("WebDAV fixture: " + str(error), file=sys.stderr)
        return 1
    except (OSError, ssl.SSLError):
        print("WebDAV fixture: could not create or clean up owned fixture resources", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
