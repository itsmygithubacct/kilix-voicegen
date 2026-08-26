#!/usr/bin/env python3
"""ABI and bounded-stream tests from the intended Python binding language."""

from __future__ import annotations

import hashlib
import os
import pathlib
import threading
import time
import unittest

from kilix_voicegen import NativeLibrary, Profile, SAMPLE_RATE, Status, VoicegenError


class BindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = NativeLibrary(os.environ["KGV_LIBRARY"])
        cls.model = pathlib.Path(os.environ["KGV_FIXTURE_DIR"])
        cls.release_sha = os.environ["KGV_FIXTURE_SHA256"]

    def engine(self):  # type annotation would expose a private binding detail
        return self.native.open_engine(self.model, self.release_sha, threads=1)

    def test_status_and_abi_surface(self) -> None:
        for status in Status:
            self.assertEqual(self.native.status_name(int(status)), f"KGV_{status.name}")
        self.assertEqual(self.native.status_name(9999), "KGV_UNKNOWN_STATUS")

    def test_deterministic_bounded_stream_for_both_voices(self) -> None:
        digests: dict[str, str] = {}
        with self.engine() as engine:
            for voice in ("kilix-female-01", "kilix-male-01"):
                blocks = list(engine.stream_pcm(
                    "Kilix fixture", voice_id=voice, profile=Profile.PROSE, queue_blocks=2))
                self.assertGreater(len(blocks), 2)
                self.assertTrue(all(0 < len(block) <= 480 * 2 for block in blocks))
                digest = hashlib.sha256(b"".join(blocks)).hexdigest()
                repeated = hashlib.sha256(b"".join(engine.stream_pcm(
                    "Kilix fixture", voice_id=voice, profile=Profile.PROSE))).hexdigest()
                self.assertEqual(digest, repeated)
                digests[voice] = digest
        self.assertNotEqual(digests["kilix-female-01"], digests["kilix-male-01"])

    def test_whitespace_has_no_callback(self) -> None:
        with self.engine() as engine:
            self.assertEqual(list(engine.stream_pcm(
                " \t\r\n", voice_id="kilix-female-01", profile=Profile.PROSE)), [])

    def test_callback_backpressure_is_synchronous(self) -> None:
        with self.engine() as engine, engine.create_job(
                "backpressure", voice_id="kilix-female-01", profile=Profile.PROSE) as job:
            calls = 0

            def slow_consumer(block: bytes, sample_rate: int) -> bool:
                nonlocal calls
                self.assertEqual(sample_rate, SAMPLE_RATE)
                self.assertGreater(len(block), 0)
                calls += 1
                time.sleep(0.004)
                return True

            started = time.monotonic()
            job.run(slow_consumer)
            elapsed = time.monotonic() - started
            self.assertGreaterEqual(calls, 3)
            self.assertGreaterEqual(elapsed, calls * 0.003)

    def test_cancel_from_another_thread(self) -> None:
        with self.engine() as engine:
            job = engine.create_job(
                "cancel from control thread", voice_id="kilix-male-01", profile=Profile.PROSE)
            entered = threading.Event()
            result: list[Status] = []

            def consumer(_block: bytes, _sample_rate: int) -> bool:
                entered.set()
                time.sleep(0.02)
                return True

            def run() -> None:
                try:
                    job.run(consumer)
                except VoicegenError as error:
                    result.append(error.status)

            thread = threading.Thread(target=run)
            thread.start()
            self.assertTrue(entered.wait(1.0))
            job.cancel()
            job.cancel()
            thread.join(1.0)
            self.assertFalse(thread.is_alive())
            self.assertEqual(result, [Status.CANCELLED])
            job.close()

    def test_destroy_waits_for_running_callback_and_cancels(self) -> None:
        with self.engine() as engine:
            job = engine.create_job(
                "destroy lifecycle", voice_id="kilix-female-01", profile=Profile.PROSE)
            entered = threading.Event()
            result: list[Status] = []

            def consumer(_block: bytes, _sample_rate: int) -> bool:
                entered.set()
                time.sleep(0.04)
                return True

            def run() -> None:
                try:
                    job.run(consumer)
                except VoicegenError as error:
                    result.append(error.status)

            thread = threading.Thread(target=run)
            thread.start()
            self.assertTrue(entered.wait(1.0))
            started = time.monotonic()
            job.close()
            elapsed = time.monotonic() - started
            thread.join(1.0)
            self.assertFalse(thread.is_alive())
            self.assertGreaterEqual(elapsed, 0.02)
            self.assertEqual(result, [Status.CANCELLED])

    def test_abandoning_bounded_iterator_cancels(self) -> None:
        with self.engine() as engine:
            stream = engine.stream_pcm(
                "abandon this stream", voice_id="kilix-female-01",
                profile=Profile.PROSE, queue_blocks=1)
            self.assertGreater(len(next(stream)), 0)
            started = time.monotonic()
            stream.close()
            self.assertLess(time.monotonic() - started, 1.0)

    def test_busy_and_visible_input_errors(self) -> None:
        with self.engine() as engine:
            first = engine.create_job(
                "one", voice_id="kilix-female-01", profile=Profile.PROSE)
            with self.assertRaises(VoicegenError) as busy:
                engine.create_job("two", voice_id="kilix-male-01", profile=Profile.PROSE)
            self.assertEqual(busy.exception.status, Status.BUSY)
            first.close()

            cases = [
                (b"\xc3(", Status.INVALID_TEXT),
                (b"a\x00b", Status.INVALID_TEXT),
                (b"a" * 65537, Status.INPUT_TOO_LARGE),
            ]
            for text, expected in cases:
                with self.subTest(expected=expected):
                    with self.assertRaises(VoicegenError) as caught:
                        engine.create_job(
                            text, voice_id="kilix-female-01", profile=Profile.PROSE)
                    self.assertEqual(caught.exception.status, expected)
                    self.assertNotIn("a" * 32, caught.exception.detail)

    def test_job_creation_runs_the_verified_resolved_frontend(self) -> None:
        with self.engine() as engine:
            with self.assertRaises(VoicegenError) as unknown:
                engine.create_job(
                    "caf\N{LATIN SMALL LETTER E WITH ACUTE}",
                    voice_id="kilix-female-01", profile=Profile.PROSE)
            self.assertEqual(unknown.exception.status, Status.INVALID_TEXT)

            with self.assertRaises(VoicegenError) as oversized_surface:
                engine.create_job(
                    "a" * 4097, voice_id="kilix-female-01",
                    profile=Profile.PROSE)
            self.assertEqual(oversized_surface.exception.status,
                             Status.INPUT_TOO_LARGE)


if __name__ == "__main__":
    unittest.main()
