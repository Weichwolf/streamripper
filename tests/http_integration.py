#!/usr/bin/env python3
"""Offline HTTP/TLS transport regressions with bounded, fragmented responses."""
import http.server
import gzip
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import time

PAYLOAD = bytes(i % 251 for i in range(250000))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except ssl.SSLError as error:
            # Aborting a transfer deliberately closes the client without TLS
            # close_notify. Other TLS errors must still fail visibly.
            if error.reason != "UNEXPECTED_EOF_WHILE_READING":
                raise

    def log_message(self, *_args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/large-meta":
            audio = (Path(__file__).parent / "fixtures" / "stereo-gap.mp3").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("icy-br", "96")
            self.send_header("icy-metaint", "45000")
            self.end_headers()
            try:
                # A bounded burst must reach disk without filling 24 large blocks.
                payload = (audio * (450000 // len(audio) + 1))[:450000]
                for start in range(0, len(payload), 45000):
                    self.wfile.write(payload[start:start + 45000] + b"\0")
                self.wfile.flush()
                time.sleep(5)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path in ("/ogg", "/opus"):
            audio = (Path(__file__).parent / "fixtures" / ("vorbis.ogg" if path == "/ogg" else "unsupported-opus.ogg")).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.end_headers()
            try:
                self.wfile.write(audio)
                self.wfile.flush()
                time.sleep(5)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path in ("/radio", "/aac", "/longradio"):
            audio = (Path(__file__).parent / "fixtures" / ("tone.aac" if path == "/aac" else "stereo-gap.mp3")).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/aac" if path == "/aac" else "audio/mpeg")
            self.send_header("icy-name", "Unicode fixture")
            self.send_header("icy-br", "96")
            self.send_header("icy-metaint", "8192")
            self.end_headers()
            position = 0
            try:
                while True:
                    block = bytes(audio[(position + i) % len(audio)] for i in range(8192))
                    title = f"Björk - Grüße 東京 🎵 {position // len(audio) + 1}"
                    if path == "/longradio":
                        title = "Ä" + "A" * 440 + " - 東京 🎵 " + "T" * 430 + str(position // len(audio))
                    metadata = f"StreamTitle='{title}';".encode()
                    # Deliberately fill every metadata byte; no NUL terminator.
                    blocks = 128 if position // 8192 % 2 else 16
                    blocks = max(blocks, (len(metadata) + 15) // 16)
                    metadata = metadata.ljust(blocks * 16, b" ")
                    self.wfile.write(block + bytes([blocks]) + metadata)
                    self.wfile.flush()
                    position += 8192
                    time.sleep(0.02)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path == "/hls":
            body = b"#EXTM3U\n#EXT-X-TARGETDURATION:5\nsegment.ts\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.apple.mpegurl")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")
            return
        if path == "/gzip":
            body = gzip.compress(PAYLOAD)
            self.send_response(200)
            self.send_header("Content-Type", "Audio/MPEG")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("icy-metaint", "8192")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/chunked")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/cycle":
            self.send_response(302)
            self.send_header("Location", "/cycle")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path in ("/pls", "/m3u", "/playlist-cycle"):
            body = (b"[playlist]\r\nFile1=/chunked\r\n" if path == "/pls" else
                    b"#EXTM3U\n/chunked?token=a=b\n" if path == "/m3u" else
                    b"[playlist]\nFile1=/playlist-cycle\n")
            self.send_response(200)
            self.send_header("Content-Type", "audio/x-scpls" if path != "/m3u" else "audio/x-mpegurl")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/missing":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/stall":
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.end_headers()
            time.sleep(3)
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Icy-MetaInt", "2147483647" if path == "/bad-meta" else "8192")
        if path == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        try:
            for start in range(0, len(PAYLOAD), 733):
                block = PAYLOAD[start:start + 733]
                if path == "/chunked":
                    self.wfile.write(f"{len(block):x};test=yes\r\n".encode() + block + b"\r\n")
                else:
                    self.wfile.write(block)
            if path == "/chunked":
                self.wfile.write(b"0\r\nX-Trailer: test\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


def run(binary, url, *, success=True, env=None):
    result = subprocess.run([binary, url], capture_output=True, timeout=10, env=env)
    assert (result.returncode == 0) == success, (url, result.returncode, result.stdout, result.stderr)
    # Failure-path tests must also be sanitizer-clean.
    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="streamripper-http-") as tmp:
        tmp = Path(tmp)
        cert, key = tmp / "cert.pem", tmp / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        servers = []
        try:
            for secure in (False, True):
                server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                servers.append(server)
                if secure:
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(cert, key)
                    server.socket = context.wrap_socket(server.socket, server_side=True)
                threading.Thread(target=server.serve_forever, daemon=True).start()
                base = f"{'https' if secure else 'http'}://localhost:{server.server_port}"
                env = dict(os.environ, CURL_CA_BUNDLE=str(cert))
                for path in ("/audio", "/chunked", "/redirect", "/pls?sid=1", "/m3u", "/gzip"):
                    run(binary, base + path, env=env)
                for path in ("/cycle", "/playlist-cycle", "/missing", "/stall", "/hls", "/json", "/bad-meta"):
                    run(binary, base + path, success=False, env=env)
                if not secure:
                    cli = sys.argv[2]
                    output = tmp / "large-meta-recording"
                    result = subprocess.run([cli, base + "/large-meta", "-d", str(output), "-l", "3"],
                        capture_output=True, timeout=10,
                        env=dict(env, XDG_CONFIG_HOME=str(tmp / "large-meta-profile")))
                    assert result.returncode == 0, (result.stdout, result.stderr)
                    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr
                    assert any(p.stat().st_size > 4096 for p in output.rglob("*.mp3") if p.is_file()), result.stdout
                    for codec, algorithm in (("radio", 1), ("radio", 2), ("aac", 1), ("longradio", 1)):
                        output = tmp / f"recordings-{codec}-{algorithm}"
                        profile = tmp / f"config-{codec}-{algorithm}"
                        args = [cli, base + "/" + codec, "-d", str(output), "-l", "25", "--codeset-metadata=auto"]
                        if algorithm == 2:
                            args.extend(["--xs2", "--xs-padding=300:300"])
                        if codec == "radio":
                            args.append("--with-id3v1")
                        if codec == "longradio":
                            args.extend(["-a", "show"])
                        # Wait for actual completed tracks rather than a CPU-sensitive
                        # six-second throughput assumption under sanitizers.
                        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                              env=dict(env, XDG_CONFIG_HOME=str(profile))) as process:
                            deadline = time.monotonic() + 35
                            while True:
                                tracks = [p for p in output.rglob("*.aac" if codec == "aac" else "*.mp3")
                                          if p.is_file() and "incomplete" not in p.parts and p.name != "show.mp3"]
                                if len(tracks) >= 2 and process.poll() is None:
                                    process.send_signal(signal.SIGINT)
                                try:
                                    stdout, stderr = process.communicate(timeout=0.1)
                                    break
                                except subprocess.TimeoutExpired:
                                    if time.monotonic() > deadline:
                                        process.kill()
                                        process.communicate()
                                        raise AssertionError("CLI recording did not stop")
                            result = subprocess.CompletedProcess(args, process.returncode, stdout, stderr)
                        assert result.returncode == 0, result.stderr
                        result.stdout.decode("utf-8")
                        assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr
                        tracks = [p for p in output.rglob("*.aac" if codec == "aac" else "*.mp3") if "incomplete" not in p.parts and p.name != "show.mp3"]
                        assert len(tracks) >= 2, (tracks, result.stdout[-2000:])
                        for track in tracks:
                            if codec != "longradio":
                                assert "Björk - Grüße 東京 🎵" in track.name, track.name
                            data = track.read_bytes()
                            if codec == "aac":
                                offset = 0
                                while offset + 7 <= len(data):
                                    assert data[offset] == 255 and data[offset + 1] & 246 == 240, (track, offset)
                                    size = ((data[offset + 3] & 3) << 11) | (data[offset + 4] << 3) | (data[offset + 5] >> 5)
                                    assert size >= 7
                                    offset += size
                                assert offset == len(data), (track, offset, len(data))
                                continue
                            assert data[:3] == b"ID3", track
                            # Parse ID3v2.3 text frames and verify complete UTF-16.
                            offset = 10
                            tags = {}
                            while offset + 10 <= len(data) and data[offset:offset + 4].isalnum():
                                tag = data[offset:offset + 4]
                                length = int.from_bytes(data[offset + 4:offset + 8], "big")
                                value = data[offset + 10:offset + 10 + length]
                                if value and value[0] == 1:
                                    tags[tag] = value[1:].decode("utf-16").rstrip("\x00")
                                offset += 10 + length
                            if codec == "longradio":
                                assert tags[b"TPE1"] == "Ä" + "A" * 440, tags
                                assert tags[b"TIT2"].startswith("東京 🎵 " + "T" * 430), tags
                            else:
                                assert tags[b"TPE1"] == "Björk", tags
                                assert tags[b"TIT2"].startswith("Grüße 東京 🎵"), tags
                                assert data[-128:-125] == b"TAG", track
                                assert data[-95:-65].rstrip(b"\0").decode("latin-1") == "Björk", track
                            # Every encoded frame must be complete, including its final byte.
                            offset = 10 + sum((data[6 + i] & 127) << (7 * (3 - i)) for i in range(4))
                            audio_end = len(data) - (128 if codec == "radio" else 0)
                            rates = (44100, 48000, 32000)
                            bitrates = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
                            while offset + 4 <= audio_end:
                                assert data[offset] == 255 and data[offset + 1] & 224 == 224, (track, offset)
                                info = data[offset + 2]
                                size = 144000 * bitrates[info >> 4] // rates[(info >> 2) & 3] + ((info >> 1) & 1)
                                assert size > 0
                                offset += size
                            assert offset == audio_end, (track, offset, audio_end)
                if not secure:
                    result = subprocess.run([sys.argv[2], base + "/stall", "-l", "1"],
                        capture_output=True, timeout=6, env=dict(env, XDG_CONFIG_HOME=str(tmp / "cancel-profile")))
                    assert result.returncode == 0 and b"Sanitizer" not in result.stderr, result.stderr
                if not secure:
                    output = tmp / "ogg-recording"
                    result = subprocess.run([sys.argv[2], base + "/ogg", "-d", str(output),
                        "-l", "2", "-a", "vorbis-test", "-A"], capture_output=True,
                        timeout=12, env=dict(env, XDG_CONFIG_HOME=str(tmp / "ogg-profile")))
                    assert result.returncode == 0, (result.stdout, result.stderr)
                    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr
                    recordings = list(output.rglob("*.ogg"))
                    assert recordings, (list(output.rglob("*")), result.stdout)
                    assert any(p.read_bytes().startswith(b"OggS") for p in recordings), recordings
                if not secure:
                    result = subprocess.run([sys.argv[2], base + "/opus", "-l", "2", "-d", str(tmp / "opus")],
                        capture_output=True, timeout=8, env=dict(env, XDG_CONFIG_HOME=str(tmp / "opus-profile")))
                    assert result.returncode == 1 and b"Unsupported stream format" in result.stderr, result.stderr
                    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr
                if secure:
                    run(binary, base + "/audio", success=False, env=dict(os.environ, CURL_CA_BUNDLE=""))
            print("HTTP/HTTPS: redirects, PLS/M3U, chunking, backpressure, TLS verification and failures passed")
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
