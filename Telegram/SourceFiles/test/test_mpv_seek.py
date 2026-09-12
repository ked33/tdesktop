#!/usr/bin/env python3
"""Exercise MPV seeking with the production options and MP4 header patches.

The local HTTP transport does not exercise Telegram's Reader or MTProto.
Pass the compiled test_mp4_header executable, MPV, and FFmpeg explicitly.
No MPV configuration is loaded, and video/audio output remains disabled.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(command: list[str], timeout: float = 60) -> str:
    result = subprocess.run(
        command, check=True, capture_output=True, text=True,
        timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    return result.stdout


def atoms(data: bytes | bytearray, start: int = 0, end: int | None = None):
    end = len(data) if end is None else end
    while start < end:
        if end - start < 8:
            raise ValueError("Truncated atom")
        size, kind = struct.unpack_from(">I4s", data, start)
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, start + 8)[0]
            header = 16
        elif size == 0:
            size = end - start
        if size < header or size > end - start:
            raise ValueError("Invalid atom size")
        yield start, size, kind, header
        start += size


def chunk_offsets(data: bytes | bytearray, start: int = 0, end: int | None = None):
    for offset, size, kind, header in atoms(data, start, end):
        if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl"):
            yield from chunk_offsets(data, offset + header, offset + size)
        elif kind in (b"stco", b"co64"):
            width, form = (4, ">I") if kind == b"stco" else (8, ">Q")
            count = struct.unpack_from(">I", data, offset + header + 4)[0]
            if header + 8 + count * width != size:
                raise ValueError("Invalid chunk table")
            for index in range(count):
                position = offset + header + 8 + index * width
                yield position, form, struct.unpack_from(form, data, position)[0]


def large_moov(data: bytes) -> bytes:
    result = bytearray(data)
    offset, size, _, header = next(a for a in atoms(data) if a[2] == b"moov")
    if header != 8:
        raise ValueError("Fixture requires a 32-bit moov")
    padding = 3 * 1024 * 1024
    insertion = offset + size
    for position, form, value in chunk_offsets(data):
        if value >= insertion:
            struct.pack_into(form, result, position, value + padding)
    struct.pack_into(">I", result, offset, size + padding)
    result[insertion:insertion] = struct.pack(">I4s", padding, b"free") + bytes(padding - 8)
    return bytes(result)


def multiple_mdat(data: bytes) -> bytes:
    result = bytearray(data)
    offset, size, _, header = next(a for a in atoms(data) if a[2] == b"mdat")
    if header != 8:
        raise ValueError("Fixture requires a 32-bit mdat")
    table = list(chunk_offsets(data))
    split = next(value for value in sorted({v for _, _, v in table})
        if offset + size // 2 < value < offset + size)
    for position, form, value in table:
        if value >= split:
            struct.pack_into(form, result, position, value + 8)
    struct.pack_into(">I", result, offset, split - offset)
    result[split:split] = struct.pack(">I4s", offset + size - split + 8, b"mdat")
    return bytes(result)


def fixtures(ffmpeg: str, output: Path) -> list[tuple[Path, int, bool]]:
    regular = output / "regular.mp4"
    run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-n",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", "40", "-c:v", "libx264", "-preset", "ultrafast", "-g", "50",
        "-crf", "20", "-c:a", "aac", "-b:a", "64k",
        "-movflags", "+faststart", str(regular)])
    data = regular.read_bytes()
    result = [(regular, 2, False)]
    for name, content, layout, patched in (
        ("large-moov", large_moov(data), 3, False),
        ("multiple-mdat", multiple_mdat(data), 2, True),
        ("large-moov-multiple-mdat", multiple_mdat(large_moov(data)), 3, True),
    ):
        path = output / f"{name}.mp4"
        path.write_bytes(content)
        result.append((path, layout, patched))
    for name, flags, layout in (
        ("tail-moov", None, 2),
        ("fragmented-mfra", "+frag_keyframe+empty_moov+default_base_moof", 1),
        ("fragmented-sidx", "+frag_keyframe+empty_moov+default_base_moof+global_sidx", 1),
        ("fragmented-no-index", "+frag_keyframe+empty_moov+default_base_moof+skip_trailer", 1),
    ):
        path = output / f"{name}.mp4"
        command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-n",
            "-i", str(regular), "-map", "0", "-c", "copy"]
        if flags:
            command += ["-movflags", flags]
        run(command + [str(path)])
        result.append((path, layout, False))
    return result


def production_options() -> str:
    directory = Path(__file__).resolve().parents[1] / "media" / "streaming"
    options = []
    for name in ("media_streaming_mpv.cpp", "media_streaming_mpv_special.cpp"):
        text = (directory / name).read_text(encoding="utf-8")
        body = text.split("QStringList LaunchArguments(", 1)[1].split("\n}", 1)[0]
        matches = re.findall(r'u"(--demuxer-lavf-o=[^"]+)"_q', body)
        if len(matches) != 1:
            raise AssertionError(f"Inspect the launch options in {name}")
        options.append(matches[0])
    if len(set(options)) != 1:
        raise AssertionError("The two MPV backends need separate compatibility runs")
    return options[0]


class Ipc:
    def __init__(self, endpoint: str, process: subprocess.Popen, timeout: float):
        self.buffer = b""
        self.sequence = 0
        self.transport = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if os.name == "nt":
                    self.transport = open(endpoint, "r+b", buffering=0)
                else:
                    connection = socket.socket(socket.AF_UNIX)
                    try:
                        connection.connect(endpoint)
                    except OSError:
                        connection.close()
                        raise
                    connection.settimeout(0.1)
                    self.transport = connection
                break
            except OSError:
                if process.poll() is not None:
                    raise RuntimeError("MPV exited before opening IPC")
                time.sleep(0.02)
        if self.transport is None:
            raise TimeoutError("MPV IPC startup")
        if os.name == "nt":
            import ctypes
            import msvcrt
            self.ctypes = ctypes
            self.handle = msvcrt.get_osfhandle(self.transport.fileno())
            self.peek = ctypes.WinDLL("kernel32", use_last_error=True).PeekNamedPipe
            self.peek.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong,
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
            self.peek.restype = ctypes.c_int

    def receive(self) -> bytes:
        if os.name == "nt":
            available = self.ctypes.c_ulong()
            if not self.peek(self.handle, None, 0, None, self.ctypes.byref(available), None):
                raise OSError(self.ctypes.get_last_error(), "MPV pipe closed")
            return self.transport.read(available.value) if available.value else b""
        try:
            data = self.transport.recv(65536)
        except socket.timeout:
            return b""
        if not data:
            raise EOFError("MPV socket closed")
        return data

    def request(self, *command, timeout: float = 5):
        self.sequence += 1
        encoded = (json.dumps({"command": command, "request_id": self.sequence}) + "\n").encode()
        if os.name == "nt":
            self.transport.write(encoded)
        else:
            self.transport.sendall(encoded)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                value = json.loads(line)
                if value.get("request_id") == self.sequence:
                    return value
            self.buffer += self.receive()
            time.sleep(0.005)
        raise TimeoutError(f"MPV IPC: {command}")

    def property(self, name: str):
        return self.request("get_property", name).get("data")

    def close(self):
        self.transport.close()


def serve_file(source: Path, metadata: dict, records: list):
    size = metadata["file_size"]
    patch = bytes(metadata["patch_bytes"])
    patch_offset = metadata["patch_offset"]

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            match = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
            start, end = 0, size - 1
            if match:
                start = int(match[1]) if match[1] else max(0, size - int(match[2]))
                end = min(size - 1, int(match[2])) if match[1] and match[2] else size - 1
            if start > end or start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            record = {"offset": start, "written": 0}
            records.append(record)
            try:
                self.send_response(206 if match else 200)
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Length", str(end - start + 1))
                self.send_header("Connection", "close")
                if match:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.end_headers()
                if body:
                    with source.open("rb") as stream:
                        stream.seek(start)
                        offset = start
                        while offset <= end:
                            data = bytearray(stream.read(min(65536, end - offset + 1)))
                            if not data:
                                raise EOFError("Fixture changed during playback")
                            begin = max(offset, patch_offset)
                            finish = min(offset + len(data), patch_offset + len(patch))
                            if begin < finish:
                                data[begin - offset:finish - offset] = patch[begin - patch_offset:finish - patch_offset]
                            self.wfile.write(data)
                            record["written"] += len(data)
                            offset += len(data)
                            time.sleep(0.001)
            except (OSError, ConnectionError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def wait_position(ipc: Ipc, target: float, timeout: float):
    deadline = time.monotonic() + timeout
    position = None
    while time.monotonic() < deadline:
        position = ipc.property("time-pos")
        if position is not None and abs(position - target) < 0.2 and ipc.property("seeking") is False:
            return
        time.sleep(0.05)
    raise TimeoutError(f"Seek to {target:.3f}; position={position}")


def check_playback(source: Path, metadata: dict, options, lavf: str) -> dict:
    records = []
    server = serve_file(source, metadata, records)
    endpoint = (r"\\.\pipe\tdesktop-seek-" + uuid.uuid4().hex if os.name == "nt"
        else str(options.output / (uuid.uuid4().hex + ".sock")))
    log = options.output / f"{source.stem}-mpv.log"
    command = [options.mpv, "--no-config", "--no-terminal", "--load-scripts=no",
        "--vo=null", "--ao=null", "--hwdec=no", "--cache=no",
        "--demuxer-readahead-secs=0", "--pause", "--idle=yes", "--keep-open=yes",
        "--force-window=no", "--save-position-on-quit=no", lavf,
        "--msg-level=all=v", f"--log-file={log}", f"--input-ipc-server={endpoint}",
        f"http://127.0.0.1:{server.server_port}/video.mp4"]
    process = None
    ipc = None
    started = time.monotonic()
    result = {"file": source.name, "layout": metadata["layout"],
        "patched": bool(metadata["patch_bytes"]), "seeks": []}
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=options.output,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        ipc = Ipc(endpoint, process, 5)
        deadline = started + options.timeout
        while time.monotonic() < deadline:
            if ipc.property("time-pos") is not None and not ipc.property("seeking"):
                break
            time.sleep(0.05)
        else:
            raise TimeoutError("MPV startup")
        result["startup_seconds"] = round(time.monotonic() - started, 3)
        result["startup_requests"] = len(records)
        result["startup_written_bytes"] = sum(x["written"] for x in records)
        if ipc.property("seekable") is not True:
            raise AssertionError("MPV considers the HTTP stream unseekable")
        duration = ipc.property("duration")
        if not duration or duration <= 1:
            raise AssertionError("Missing media duration")
        for fraction in (0.9, 0.15, 0.95, 0.4, 0.85):
            target = duration * fraction
            seek_started = time.monotonic()
            before = len(records)
            response = ipc.request("seek", target, "absolute+exact")
            if response.get("error") != "success":
                raise AssertionError(f"Seek command failed: {response}")
            wait_position(ipc, target, options.timeout)
            count = len(records) - before
            result["seeks"].append({"target": round(target, 3),
                "seconds": round(time.monotonic() - seek_started, 3), "requests": count})
            if fraction == 0.9 and count == 0:
                raise AssertionError("The first distant seek did not exercise HTTP Range")
        seek_started = time.monotonic()
        before = len(records)
        for fraction in (0.1, 0.8, 0.25, 0.65, 0.05, 0.92):
            target = duration * fraction
            response = ipc.request("seek", target, "absolute+exact")
            if response.get("error") != "success":
                raise AssertionError(f"Rapid seek command failed: {response}")
        wait_position(ipc, target, options.timeout)
        result["rapid_seek"] = {"commands": 6, "target": round(target, 3),
            "seconds": round(time.monotonic() - seek_started, 3),
            "requests": len(records) - before}
        result["passed"] = True
    except Exception as error:
        result["passed"] = False
        result["error"] = str(error)
    finally:
        if ipc:
            try:
                ipc.request("quit", timeout=1)
            except (OSError, EOFError, TimeoutError):
                pass
        if process:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=3)
        if ipc:
            ipc.close()
        server.shutdown()
        server.server_close()
    result["requests"] = len(records)
    result["written_bytes"] = sum(x["written"] for x in records)
    (options.output / f"{source.stem}-requests.json").write_text(
        json.dumps(records, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpv", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--header-test", required=True)
    parser.add_argument("--sample", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=20)
    options = parser.parse_args()
    options.output = options.output or Path(tempfile.mkdtemp(prefix="tdesktop-mpv-seek-"))
    options.output = options.output.resolve()
    for attribute in ("mpv", "ffmpeg", "header_test"):
        path = Path(getattr(options, attribute))
        if path.exists():
            setattr(options, attribute, str(path.resolve()))
    options.output.mkdir(parents=True, exist_ok=True)
    print(json.dumps({"output": str(options.output)}), flush=True)
    lavf = production_options()
    inputs = fixtures(options.ffmpeg, options.output)
    inputs.extend((path.resolve(), None, None) for path in options.sample)
    results = []
    for source, layout, patched in inputs:
        metadata = json.loads(run([options.header_test, "--inspect", str(source)]))
        if layout is not None and metadata["layout"] != layout:
            raise AssertionError(f"Unexpected MP4 layout: {source.name}")
        if patched is not None and bool(metadata["patch_bytes"]) != patched:
            raise AssertionError(f"Unexpected header patch: {source.name}")
        results.append(check_playback(source, metadata, options, lavf))
    summary = {"lavf": lavf, "results": results,
        "passed": all(result["passed"] for result in results)}
    (options.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
