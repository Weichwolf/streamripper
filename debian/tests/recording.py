#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the installed Debian executable with verified HTTPS and ICY titles."""

import http.server
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from http_integration import Handler


def main():
    with tempfile.TemporaryDirectory(prefix="streamripper-installed-") as directory:
        temporary = Path(directory)
        cert, key = temporary / "cert.pem", temporary / "key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(key), "-out", str(cert)
        ], check=True, capture_output=True)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            arguments = [
                "/usr/bin/streamripper", f"https://localhost:{server.server_port}/radio",
                "-d", str(temporary / "recordings"), "-l", "20", "--codeset-metadata=auto"
            ]
            with subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(
                os.environ, CURL_CA_BUNDLE=str(cert), XDG_CONFIG_HOME=str(temporary / "config")
            )) as process:
                deadline = time.monotonic() + 30
                while True:
                    tracks = [p for p in (temporary / "recordings").rglob("*.mp3")
                              if p.is_file() and "incomplete" not in p.parts]
                    if tracks and process.poll() is None:
                        process.send_signal(signal.SIGINT)
                    try:
                        stdout, stderr = process.communicate(timeout=0.1)
                        break
                    except subprocess.TimeoutExpired:
                        if time.monotonic() > deadline:
                            process.kill()
                            process.communicate()
                            raise AssertionError("Installed executable did not stop")
                result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
            assert result.returncode == 0, (result.stdout, result.stderr)
            result.stdout.decode("utf-8")
            tracks = [p for p in (temporary / "recordings").rglob("*.mp3")
                      if p.is_file() and "incomplete" not in p.parts]
            assert tracks, result.stdout
            for track in tracks:
                assert "Björk - Grüße 東京 🎵" in track.name, track
                data = track.read_bytes()
                assert len(data) > 4096 and data.startswith(b"ID3"), track
            print("Installed /usr/bin/streamripper: HTTPS, ICY, Unicode and MP3 files passed")
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
