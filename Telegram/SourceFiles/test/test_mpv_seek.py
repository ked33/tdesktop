#!/usr/bin/env python3
"""Exercise MPV seeking with the production options and MP4 header patches.

The local HTTP transport does not exercise Telegram's Reader or MTProto.
Pass the compiled test_mp4_header executable, MPV, and FFmpeg explicitly.
No MPV configuration is loaded, and video/audio output remains disabled.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import re
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


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
    path = output / "fragmented-b-frames.mp4"
    run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-n",
        "-i", str(regular), "-map", "0", "-c:v", "libx264", "-preset", "veryfast",
        "-bf", "3", "-g", "50", "-crf", "20", "-c:a", "copy", "-movflags",
        "+frag_keyframe+empty_moov+default_base_moof+skip_trailer", str(path)])
    result.append((path, 1, False))
    return result


def production_options() -> tuple[str, str]:
    directory = Path(__file__).resolve().parents[1] / "media" / "streaming"
    for name in ("media_streaming_mpv.cpp", "media_streaming_mpv_special.cpp"):
        text = (directory / name).read_text(encoding="utf-8")
        body = text.split("QStringList LaunchArguments(", 1)[1].split("\n}", 1)[0]
        if "PlaybackDemuxerOptions(LooksLikeMp4Stream(document))" not in body:
            raise AssertionError(f"Inspect the launch options in {name}")
    text = (directory / "media_streaming_mpv_index.cpp").read_text(encoding="utf-8")
    body = text.split("QString PlaybackDemuxerOptions(", 1)[1].split("\n}", 1)[0]
    options = re.findall(r'u"(ignore_editlist=[^"]+)"_q', body)
    if len(options) != 2:
        raise AssertionError("Inspect the fast-open and indexed demuxer options")
    return tuple("--demuxer-lavf-o=" + value for value in options)


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


class RemoteBlocks:
    def __init__(self, latency: float):
        self.latency = latency
        self.stopped = threading.Event()
        self.records = []
        self.blocks = {role: collections.OrderedDict() for role in ("media", "index")}
        self.locks = {role: threading.Lock() for role in self.blocks}

    def read(self, offset: int, count: int, role: str = "media", cancelled=None):
        def stopped():
            return self.stopped.is_set() or (cancelled and cancelled())

        part = 128 * 1024
        with self.locks[role]:
            blocks = self.blocks[role]
            for block in range(offset // part, (offset + count - 1) // part + 1):
                if stopped():
                    raise ConnectionAbortedError("Playback test stopped")
                if block not in blocks:
                    self.records.append({"role": role, "offset": block * part})
                    deadline = time.monotonic() + self.latency
                    while time.monotonic() < deadline:
                        self.stopped.wait(min(0.01, deadline - time.monotonic()))
                        if stopped():
                            raise ConnectionAbortedError("Playback test stopped")
                blocks[block] = True
                blocks.move_to_end(block)
                if len(blocks) > 128:
                    blocks.popitem(last=False)


class PreparedIndex:
    def __init__(self, source: Path, metadata: dict, remote: RemoteBlocks):
        self.source = source
        self.metadata = metadata
        self.remote = remote
        self.ready = threading.Event()
        self.ranges = metadata["ranges"]
        self.offsets = [offset for offset, _ in self.ranges]
        self.seconds = None
        self.worker = threading.Thread(target=self.prepare, daemon=True)

    def prepare(self):
        started = time.monotonic()
        try:
            for offset, count in self.metadata["reads"]:
                self.remote.read(offset, count, "index")
        except ConnectionAbortedError:
            return
        self.seconds = round(time.monotonic() - started, 3)
        self.ready.set()

    def count(self, offset: int, maximum: int) -> int:
        if not self.ready.is_set():
            return 0
        found = bisect.bisect_right(self.offsets, offset) - 1
        if found < 0:
            return 0
        begin, length = self.ranges[found]
        return max(0, min(maximum, begin + length - offset))


class OnDemandIndex:
    def __init__(self, source: Path, duration_ms: int, executable: str,
            initial: dict, remote: RemoteBlocks):
        self.source = source
        self.duration_ms = duration_ms
        self.executable = executable
        self.initial = initial
        self.remote = remote
        self.revision = 0
        self.position = 0
        self.patches = {}
        self.records = []
        self.error = None
        self.lock = threading.Lock()
        self.changed = threading.Event()
        self.worker = threading.Thread(target=self.prepare, daemon=True)

    def request(self, position: float) -> int:
        with self.lock:
            self.revision += 1
            self.position = round(position * 1000)
            self.changed.set()
            return self.revision

    def seek_patches(self, revision: int):
        with self.lock:
            return self.patches.get(revision)

    def prepare(self):
        revision = 0
        plan = self.initial
        while not self.remote.stopped.is_set():
            started = time.monotonic()
            try:
                for offset, count in plan["reads"]:
                    self.remote.read(offset, count, "index",
                        cancelled=lambda: self.revision != revision)
                if revision:
                    if not plan["ready"]:
                        raise AssertionError("On-demand lookup fell back to a full scan")
                    with self.lock:
                        if self.revision == revision:
                            self.patches[revision] = plan["patches"]
                            self.records.append({"revision": revision,
                                "position_ms": self.position,
                                "seconds": round(time.monotonic() - started, 3),
                                "read_bytes": sum(count for _, count in plan["reads"])})
            except ConnectionAbortedError:
                pass
            except Exception as error:
                self.error = error
                return
            while not self.remote.stopped.is_set():
                self.changed.wait(.1)
                with self.lock:
                    if self.revision != revision:
                        revision, position = self.revision, self.position
                        self.changed.clear()
                        break
            else:
                return
            try:
                plan = json.loads(run([self.executable, "--seek", str(self.source),
                    str(self.duration_ms), str(position)]))
            except Exception as error:
                self.error = error
                return


def serve_file(source: Path, metadata: dict, records: list,
        remote: RemoteBlocks | None = None, index: PreparedIndex | None = None,
        seek_patches=None):
    source_size = metadata["file_size"]
    patches = [(metadata["patch_offset"], bytes(metadata["patch_bytes"]))]
    patches.extend((value["offset"], bytes(value["bytes"]))
        for value in metadata.get("duration_patches", []))

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def disconnected(self):
            try:
                readable, _, _ = select.select([self.connection], [], [], 0)
                return bool(readable and not self.connection.recv(1, socket.MSG_PEEK))
            except OSError:
                return True

        def serve(self, body):
            revision = int(parse_qs(urlsplit(self.path).query).get("tdesktop_index", ["0"])[0])
            prepared = seek_patches(revision) if seek_patches and revision else []
            if prepared is None:
                self.send_error(404)
                return
            current_patches = patches + [(p["offset"], bytes(p["bytes"])) for p in prepared]
            size = source_size
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
            record = {"offset": start, "written": 0, "metadata_bytes": 0}
            records.append(record)
            self.close_connection = True
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
                        stream.seek(min(start, source_size))
                        offset = start
                        while offset <= end:
                            count = min(65536, end - offset + 1)
                            cached = index.count(offset, count) if index else 0
                            if cached:
                                count = cached
                                record["metadata_bytes"] += cached
                            elif remote:
                                remote.read(offset, count, cancelled=self.disconnected)
                            data = bytearray(stream.read(count))
                            if not data:
                                raise EOFError("Fixture changed during playback")
                            for patch_offset, patch in current_patches:
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


def wait_position(ipc: Ipc, target: float, timeout: float, progress=None,
        require_audio=True):
    deadline = time.monotonic() + timeout
    position = None
    audio = None
    has_audio = require_audio and any(t.get("type") == "audio" and t.get("selected")
        for t in ipc.property("track-list") or [])
    stable_since = None
    while time.monotonic() < deadline:
        position = ipc.property("time-pos")
        audio = ipc.property("audio-pts") if has_audio else target
        if (position is not None and abs(position - target) < 0.2
                and audio is not None and abs(audio - target) < 0.2
                and ipc.property("seeking") is False):
            stable_since = stable_since or time.monotonic()
            if time.monotonic() - stable_since >= 0.3:
                return
        else:
            stable_since = None
        if progress:
            progress()
        time.sleep(0.05)
    raise TimeoutError(f"Seek to {target:.3f}; position={position}, audio={audio}")


def check_decoding(ipc: Ipc, target: float, timeout: float):
    tracks = ipc.property("track-list") or []
    has_audio = any(t.get("type") == "audio" and t.get("selected") for t in tracks)
    stable_since = None
    ipc.request("set_property", "pause", False)
    deadline = time.monotonic() + min(timeout, 8)
    try:
        while time.monotonic() < deadline:
            position = ipc.property("time-pos")
            audio = ipc.property("audio-pts") if has_audio else position
            if (position is not None and target - 0.2 <= position <= target + 2
                    and audio is not None and abs(audio - position) < 0.3
                    and ipc.property("seeking") is False):
                stable_since = stable_since or time.monotonic()
                if time.monotonic() - stable_since >= 0.3:
                    return
            else:
                stable_since = None
            time.sleep(0.05)
        raise TimeoutError(f"Playback after seek to {target:.3f}; position={position}, audio={audio}")
    finally:
        ipc.request("set_property", "pause", True)


def check_playback(source: Path, metadata: dict, options,
        lavf: str, indexed_lavf: str) -> dict:
    records = []
    remote = RemoteBlocks(options.latency_ms / 1000)
    index = None
    if metadata["layout"] == 1 and not options.previous_behavior:
        plan = json.loads(run([options.index_test, "--seek", str(source),
            str(metadata["duration_ms"]), "0"]))
        if plan["on_demand"]:
            index = OnDemandIndex(source, metadata["duration_ms"], options.index_test, plan, remote)
        else:
            plan = json.loads(run([options.index_test, "--inspect", str(source)]))
            index = PreparedIndex(source, plan, remote)
        index.worker.start()
    on_demand = isinstance(index, OnDemandIndex)
    server = serve_file(source, metadata, records, remote,
        None if on_demand else index, index.seek_patches if on_demand else None)
    endpoint = (r"\\.\pipe\tdesktop-seek-" + uuid.uuid4().hex if os.name == "nt"
        else str(options.output / (uuid.uuid4().hex + ".sock")))
    log = options.output / f"{source.stem}-mpv.log"
    url = f"http://127.0.0.1:{server.server_port}/video.mp4"
    command = [options.mpv, "--no-config", "--no-terminal", "--load-scripts=no",
        "--vo=null", "--ao=null", "--hwdec=no", "--cache=no",
        "--demuxer-readahead-secs=0", "--pause", "--idle=yes", "--keep-open=yes",
        "--force-window=no", "--save-position-on-quit=no", lavf,
        "--msg-level=all=v", f"--log-file={log}", f"--input-ipc-server={endpoint}",
        url]
    process = None
    ipc = None
    started = time.monotonic()
    result = {"file": source.name, "layout": metadata["layout"],
        "patched": bool(metadata["patch_bytes"]), "on_demand": on_demand, "seeks": []}
    active_target = None
    revision = 0
    loaded_revision = 0

    def prepare_seek():
        nonlocal revision, loaded_revision
        if on_demand:
            if index.error:
                raise index.error
            if not revision:
                revision = index.request(active_target)
            if index.seek_patches(revision) is not None and loaded_revision != revision:
                paused = ipc.property("pause")
                response = ipc.request("loadfile", url + "?tdesktop_index=" + str(revision),
                    "replace", -1, {"start": str(active_target),
                        "pause": "yes" if paused else "no",
                        "demuxer-lavf-o": lavf.split("=", 1)[1]})
                if response.get("error") != "success":
                    raise AssertionError(f"On-demand reload failed: {response}")
                loaded_revision = revision
                result["index_reloads"] = result.get("index_reloads", 0) + 1
            return
        if not index or not index.ready.is_set() or result.get("index_reloaded"):
            return
        if not ipc.property("seeking") or ipc.property("path") != url:
            return
        target = ipc.property("time-pos")
        paused = ipc.property("pause")
        response = ipc.request("loadfile", url, "replace", -1, {
            "start": str(target), "pause": "yes" if paused else "no",
            "demuxer-lavf-o": indexed_lavf.split("=", 1)[1],
        })
        if response.get("error") != "success":
            raise AssertionError(f"Indexed reload failed: {response}")
        result["index_reloaded"] = True
        result["reload_target"] = target

    try:
        environment = os.environ.copy()
        for name in list(environment):
            if name.lower() in ("http_proxy", "https_proxy", "all_proxy"):
                del environment[name]
        environment["NO_PROXY"] = "127.0.0.1,localhost"
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=options.output, env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        ipc = Ipc(endpoint, process, 5)
        deadline = started + options.startup_timeout
        while time.monotonic() < deadline:
            if ipc.property("time-pos") is not None and not ipc.property("seeking"):
                break
            time.sleep(0.05)
        else:
            raise TimeoutError("MPV startup")
        result["startup_seconds"] = round(time.monotonic() - started, 3)
        result["startup_requests"] = len(records)
        result["startup_written_bytes"] = sum(x["written"] for x in records)
        result["index_ready_at_startup"] = bool(index and not on_demand and index.ready.is_set())
        if ipc.property("seekable") is not True:
            raise AssertionError("MPV considers the HTTP stream unseekable")
        duration = ipc.property("duration")
        if not duration or duration <= 1:
            raise AssertionError("Missing media duration")
        for fraction in (0.9, 0.15, 0.95, 0.4, 0.85):
            target = duration * fraction
            active_target, revision = target, 0
            seek_started = time.monotonic()
            before = len(records)
            response = ipc.request("seek", target, "absolute+exact")
            if response.get("error") != "success":
                raise AssertionError(f"Seek command failed: {response}")
            wait_position(ipc, target, options.timeout, prepare_seek, require_audio=False)
            if ipc.property("pause") is not True:
                raise AssertionError("The indexed reload changed the pause state")
            check_decoding(ipc, target, options.timeout)
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
        active_target, revision = target, 0
        wait_position(ipc, target, options.timeout, prepare_seek, require_audio=False)
        check_decoding(ipc, target, options.timeout)
        result["rapid_seek"] = {"commands": 6, "target": round(target, 3),
            "seconds": round(time.monotonic() - seek_started, 3),
            "requests": len(records) - before}
        active_target, revision = duration * 0.4, 0
        ipc.request("seek", active_target, "absolute+exact")
        wait_position(ipc, active_target, options.timeout, prepare_seek, require_audio=False)
        check_decoding(ipc, active_target, options.timeout)
        ipc.request("set_property", "pause", False)
        time.sleep(3)
        continued = ipc.property("time-pos")
        if continued is None or continued < active_target + 2 or ipc.property("eof-reached"):
            raise AssertionError("Playback stopped after the indexed seek")
        audio = ipc.property("audio-pts")
        has_audio = any(t.get("type") == "audio" and t.get("selected")
            for t in ipc.property("track-list") or [])
        if has_audio and (audio is None or abs(audio - continued) > 0.3):
            raise AssertionError("Audio and video diverged after the indexed seek")
        result["continued_to"] = round(continued, 3)
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
        remote.stopped.set()
        if index:
            index.worker.join(timeout=3)
        server.shutdown()
        server.server_close()
    result["requests"] = len(records)
    result["written_bytes"] = sum(x["written"] for x in records)
    result["remote_blocks"] = len(remote.records)
    result["metadata_served_bytes"] = sum(x["metadata_bytes"] for x in records)
    if on_demand:
        result["index_preparations"] = index.records
    elif index:
        result["metadata_cache_bytes"] = index.metadata["size"]
        result["metadata_prepare_seconds"] = index.seconds
    (options.output / f"{source.stem}-requests.json").write_text(
        json.dumps(records, indent=2), encoding="utf-8")
    (options.output / f"{source.stem}-remote.json").write_text(
        json.dumps(remote.records, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpv", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--header-test", required=True)
    parser.add_argument("--index-test", required=True)
    parser.add_argument("--sample", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--startup-timeout", type=float, default=5)
    parser.add_argument("--latency-ms", type=float, default=100)
    parser.add_argument("--skip-fixtures", action="store_true")
    parser.add_argument("--previous-behavior", action="store_true")
    options = parser.parse_args()
    options.output = options.output or Path(tempfile.mkdtemp(prefix="tdesktop-mpv-seek-"))
    options.output = options.output.resolve()
    for attribute in ("mpv", "ffmpeg", "header_test", "index_test"):
        path = Path(getattr(options, attribute))
        if path.exists():
            setattr(options, attribute, str(path.resolve()))
    options.output.mkdir(parents=True, exist_ok=True)
    print(json.dumps({"output": str(options.output)}), flush=True)
    lavf, indexed_lavf = production_options()
    if options.previous_behavior:
        lavf = indexed_lavf
    inputs = [] if options.skip_fixtures else fixtures(options.ffmpeg, options.output)
    inputs.extend((path.resolve(), None, None) for path in options.sample)
    results = []
    for source, layout, patched in inputs:
        ffprobe = Path(options.ffmpeg).with_name("ffprobe.exe" if os.name == "nt" else "ffprobe")
        details = json.loads(run([str(ffprobe), "-v", "error", "-show_entries",
            "format=duration", "-of", "json", str(source)]))
        duration_ms = round(float(details["format"]["duration"]) * 1000)
        metadata = json.loads(run([options.header_test, "--inspect", str(source),
            str(duration_ms)]))
        metadata["duration_ms"] = duration_ms
        if layout is not None and metadata["layout"] != layout:
            raise AssertionError(f"Unexpected MP4 layout: {source.name}")
        if patched is not None and bool(metadata["patch_bytes"]) != patched:
            raise AssertionError(f"Unexpected header patch: {source.name}")
        results.append(check_playback(source, metadata, options, lavf, indexed_lavf))
    summary = {"lavf": lavf, "results": results,
        "passed": all(result["passed"] for result in results)}
    (options.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
