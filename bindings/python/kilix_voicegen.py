"""Small standard-library-only ctypes binding for the Kilix Voicegen C ABI."""

from __future__ import annotations

import ctypes
import os
import queue
import struct
import sys
import threading
from collections.abc import Callable, Iterator
from enum import IntEnum
from typing import Any

ABI_VERSION = 1
SAMPLE_RATE = 24_000
DEFAULT_SEED = 0x4B696C6978564731


class Status(IntEnum):
    OK = 0
    CANCELLED = 1
    INVALID_ARGUMENT = 2
    INVALID_TEXT = 3
    INVALID_VOICE = 4
    INVALID_MODEL = 5
    ABI_MISMATCH = 6
    BUSY = 7
    RESOURCE_EXHAUSTED = 8
    INTERNAL_ERROR = 9
    HASH_MISMATCH = 10
    UNSUPPORTED_SCHEMA = 11
    UNSUPPORTED_CPU = 12
    IO_ERROR = 13
    INPUT_TOO_LARGE = 14
    INVALID_STATE = 15


class Profile(IntEnum):
    PROSE = 1
    TERMINAL = 2


class VoicegenError(RuntimeError):
    def __init__(self, status: Status, detail: str) -> None:
        self.status = status
        self.detail = detail
        super().__init__(f"KGV_{status.name}: {detail}")


class _Engine(ctypes.Structure):
    pass


class _Job(ctypes.Structure):
    pass


class PhoneSegment(ctypes.Structure):
    _fields_ = [
        ("segment_id", ctypes.c_uint16),
        ("syllable_start", ctypes.c_uint8),
        ("stress", ctypes.c_uint8),
    ]


class PronunciationOverride(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("byte_start", ctypes.c_uint32),
        ("byte_end", ctypes.c_uint32),
        ("kind", ctypes.c_uint32),
        ("replacement_utf8", ctypes.c_char_p),
        ("replacement_utf8_size", ctypes.c_size_t),
        ("phone_segments", ctypes.POINTER(PhoneSegment)),
        ("phone_segment_count", ctypes.c_size_t),
    ]


class FrontendInput(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("utf8_text", ctypes.c_char_p),
        ("utf8_size", ctypes.c_size_t),
        ("profile", ctypes.c_uint32),
        ("overrides", ctypes.POINTER(PronunciationOverride)),
        ("override_count", ctypes.c_size_t),
        ("dictionary", ctypes.c_void_p),
    ]


class EngineOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("abi_version", ctypes.c_uint32),
        ("thread_count", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("expected_release_sha256", ctypes.c_char_p),
    ]


class Request(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("frontend", FrontendInput),
        ("voice_id", ctypes.c_char_p),
        ("rate", ctypes.c_float),
        ("seed", ctypes.c_uint64),
    ]


PcmCallback = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_int16),
    ctypes.c_size_t,
    ctypes.c_uint32,
    ctypes.c_void_p,
)


class NativeLibrary:
    """Declared ABI surface; undeclared ctypes defaults are never relied on."""

    def __init__(self, path: os.PathLike[str] | str) -> None:
        self._library = ctypes.CDLL(os.fspath(path), mode=ctypes.RTLD_LOCAL)
        library = self._library
        library.kgv_abi_version.argtypes = []
        library.kgv_abi_version.restype = ctypes.c_uint32
        library.kgv_status_name.argtypes = [ctypes.c_int]
        library.kgv_status_name.restype = ctypes.c_char_p
        library.kgv_engine_open.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(EngineOptions),
            ctypes.POINTER(ctypes.POINTER(_Engine)),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.kgv_engine_open.restype = ctypes.c_int
        library.kgv_job_create.argtypes = [
            ctypes.POINTER(_Engine),
            ctypes.POINTER(Request),
            ctypes.POINTER(ctypes.POINTER(_Job)),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.kgv_job_create.restype = ctypes.c_int
        library.kgv_job_run.argtypes = [
            ctypes.POINTER(_Job),
            PcmCallback,
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.kgv_job_run.restype = ctypes.c_int
        library.kgv_job_cancel.argtypes = [ctypes.POINTER(_Job)]
        library.kgv_job_cancel.restype = None
        library.kgv_job_destroy.argtypes = [ctypes.POINTER(_Job)]
        library.kgv_job_destroy.restype = None
        library.kgv_engine_close.argtypes = [ctypes.POINTER(_Engine)]
        library.kgv_engine_close.restype = None

        if int(library.kgv_abi_version()) != ABI_VERSION:
            raise VoicegenError(Status.ABI_MISMATCH, "loaded library does not implement ABI 1")

    def status_name(self, status: int) -> str:
        raw = self._library.kgv_status_name(status)
        return raw.decode("ascii", "strict") if raw is not None else "KGV_UNKNOWN_STATUS"

    def open_engine(self, model_directory: os.PathLike[str] | str,
                    release_sha256: str, *, threads: int = 1) -> Engine:
        encoded_directory = os.fsencode(model_directory)
        encoded_sha = release_sha256.encode("ascii", "strict")
        options = EngineOptions(
            struct_size=ctypes.sizeof(EngineOptions),
            abi_version=ABI_VERSION,
            thread_count=threads,
            flags=0,
            expected_release_sha256=encoded_sha,
        )
        pointer = ctypes.POINTER(_Engine)()
        error = ctypes.create_string_buffer(512)
        status = int(self._library.kgv_engine_open(
            encoded_directory, ctypes.byref(options), ctypes.byref(pointer), error, len(error)))
        _raise_status(status, error)
        return Engine(self, pointer)


class Engine:
    def __init__(self, native: NativeLibrary, pointer: ctypes.POINTER(_Engine)) -> None:
        self._native = native
        self._pointer = pointer
        self._closed = False

    def __enter__(self) -> Engine:
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()

    def close(self) -> None:
        if not self._closed:
            self._native._library.kgv_engine_close(self._pointer)
            self._closed = True

    def create_job(self, text: str | bytes, *, voice_id: str,
                   profile: Profile, rate: float = 1.0,
                   seed: int = DEFAULT_SEED) -> Job:
        if self._closed:
            raise VoicegenError(Status.INVALID_STATE, "engine is closed")
        encoded = text.encode("utf-8", "strict") if isinstance(text, str) else bytes(text)
        text_buffer = ctypes.create_string_buffer(encoded, len(encoded) + 1)
        voice_buffer = voice_id.encode("ascii", "strict")
        frontend = FrontendInput(
            struct_size=ctypes.sizeof(FrontendInput),
            utf8_text=ctypes.cast(text_buffer, ctypes.c_char_p),
            utf8_size=len(encoded),
            profile=int(profile),
            overrides=None,
            override_count=0,
            dictionary=None,
        )
        request = Request(
            struct_size=ctypes.sizeof(Request),
            frontend=frontend,
            voice_id=voice_buffer,
            rate=rate,
            seed=seed,
        )
        pointer = ctypes.POINTER(_Job)()
        error = ctypes.create_string_buffer(512)
        status = int(self._native._library.kgv_job_create(
            self._pointer, ctypes.byref(request), ctypes.byref(pointer), error, len(error)))
        _raise_status(status, error)
        return Job(self._native, pointer)

    def stream_pcm(self, text: str | bytes, *, voice_id: str, profile: Profile,
                   rate: float = 1.0, seed: int = DEFAULT_SEED,
                   queue_blocks: int = 4) -> Iterator[bytes]:
        """Run one job on a helper thread and expose a bounded PCM iterator."""
        if queue_blocks < 1 or queue_blocks > 64:
            raise ValueError("queue_blocks must be between 1 and 64")
        job = self.create_job(text, voice_id=voice_id, profile=profile, rate=rate, seed=seed)
        blocks: queue.Queue[bytes] = queue.Queue(maxsize=queue_blocks)
        stopped = threading.Event()
        completed = threading.Event()
        outcome: list[BaseException] = []

        def accept(block: bytes, sample_rate: int) -> bool:
            if sample_rate != SAMPLE_RATE:
                outcome.append(RuntimeError("native callback used an unexpected sample rate"))
                return False
            while not stopped.is_set():
                try:
                    blocks.put(block, timeout=0.05)
                    return True
                except queue.Full:
                    continue
            return False

        def worker() -> None:
            try:
                job.run(accept)
            except VoicegenError as error:
                if error.status is not Status.CANCELLED or not stopped.is_set():
                    outcome.append(error)
            except BaseException as error:  # Preserve callback/test failures for the consumer.
                outcome.append(error)
            finally:
                job.close()
                completed.set()

        thread = threading.Thread(target=worker, name="kilix-voicegen-job", daemon=False)
        thread.start()
        try:
            while not completed.is_set() or not blocks.empty():
                try:
                    yield blocks.get(timeout=0.05)
                except queue.Empty:
                    continue
            if outcome:
                raise outcome[0]
        finally:
            stopped.set()
            job.cancel()
            thread.join()


class Job:
    def __init__(self, native: NativeLibrary, pointer: ctypes.POINTER(_Job)) -> None:
        self._native = native
        self._pointer = pointer
        self._closed = False

    def cancel(self) -> None:
        if not self._closed:
            self._native._library.kgv_job_cancel(self._pointer)

    def run(self, callback: Callable[[bytes, int], bool]) -> None:
        if self._closed:
            raise VoicegenError(Status.INVALID_STATE, "job is closed")
        callback_error: list[BaseException] = []

        def dispatch(frames: ctypes.POINTER(ctypes.c_int16), frame_count: int,
                     sample_rate: int, _user: Any) -> int:
            try:
                if frame_count <= 0:
                    raise RuntimeError("native runtime emitted an empty PCM callback")
                if sys.byteorder == "little":
                    block = ctypes.string_at(frames, frame_count * 2)
                else:
                    block = b"".join(struct.pack("<h", frames[index])
                                     for index in range(frame_count))
                return 1 if callback(block, sample_rate) else 0
            except BaseException as error:
                callback_error.append(error)
                return 0

        native_callback = PcmCallback(dispatch)
        error = ctypes.create_string_buffer(512)
        status = int(self._native._library.kgv_job_run(
            self._pointer, native_callback, None, error, len(error)))
        if callback_error:
            raise callback_error[0]
        _raise_status(status, error)

    def close(self) -> None:
        if not self._closed:
            self._native._library.kgv_job_destroy(self._pointer)
            self._closed = True

    def __enter__(self) -> Job:
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()


def _raise_status(status_value: int, error: ctypes.Array[ctypes.c_char]) -> None:
    if status_value == int(Status.OK):
        return
    try:
        status = Status(status_value)
    except ValueError as caught:
        raise RuntimeError(f"native runtime returned unknown status {status_value}") from caught
    detail = error.value.decode("utf-8", "replace")
    raise VoicegenError(status, detail)


__all__ = [
    "ABI_VERSION", "DEFAULT_SEED", "SAMPLE_RATE", "Engine", "Job", "NativeLibrary",
    "Profile", "Status", "VoicegenError",
]
