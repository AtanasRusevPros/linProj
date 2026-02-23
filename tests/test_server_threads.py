"""Tests for server -t flag and auto-detected thread configuration.

These tests launch their own short-lived server instances and do NOT use
the session-scoped server fixture (which holds the IPC shared memory).
They must run in a separate pytest invocation or after the session server
has been torn down.
"""
import os
import signal
import subprocess
import time
import ctypes

import pytest

BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "build")
SERVER_BIN = os.path.join(BUILD_DIR, "server")
SHM_PATH = "/dev/shm/ipc_shm"
LOCK_FILE = "/tmp/ipc_server.lock"
LIBIPC_SO = os.path.join(BUILD_DIR, "libipc.so")
IPC_MAX_SLOTS = 16


def _cleanup_ipc():
    """Remove leftover IPC objects and lock file so a fresh server can start."""
    if os.path.exists(SHM_PATH):
        os.remove(SHM_PATH)
    for i in range(16):
        path = f"/dev/shm/sem.ipc_slot_{i}"
        if os.path.exists(path):
            os.remove(path)
    for name in ("sem.ipc_mutex", "sem.ipc_server_notify"):
        path = f"/dev/shm/{name}"
        if os.path.exists(path):
            os.remove(path)
    if os.path.exists(LOCK_FILE):
        os.remove(LOCK_FILE)


def _start_server(*extra_args):
    _cleanup_ipc()
    proc = subprocess.Popen(
        [SERVER_BIN, *extra_args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=BUILD_DIR,
    )
    for _ in range(50):
        if os.path.exists(SHM_PATH):
            break
        time.sleep(0.1)
    time.sleep(0.2)
    return proc


def _stop_server(proc):
    proc.send_signal(signal.SIGINT)
    try:
        stdout, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
    return stdout.decode()


def _load_ipc_lib():
    """Load libipc and configure function signatures used by tests."""
    lib = ctypes.CDLL(LIBIPC_SO)

    lib.ipc_init.argtypes = []
    lib.ipc_init.restype = ctypes.c_int

    lib.ipc_cleanup.argtypes = []
    lib.ipc_cleanup.restype = None

    lib.ipc_add.argtypes = [ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)]
    lib.ipc_add.restype = ctypes.c_int

    lib.ipc_concat.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)
    ]
    lib.ipc_concat.restype = ctypes.c_int

    return lib


class TestServerThreadConfig:
    """Test the -t flag and startup banner thread info."""

    def test_flag_override(self):
        """Server started with -t 4 should report threads/pool=4."""
        proc = _start_server("-t", "4")
        try:
            output = _stop_server(proc)
            assert "threads/pool=4" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_flag_single_thread(self):
        """Server started with -t 1 should report threads/pool=1."""
        proc = _start_server("-t", "1")
        try:
            output = _stop_server(proc)
            assert "threads/pool=1" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_banner_shows_cores(self):
        """Startup banner should include cores= field."""
        proc = _start_server("-t", "1")
        try:
            output = _stop_server(proc)
            assert "cores=" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_default_auto_detection(self):
        """Without -t, threads/pool should be auto-detected (>= 1)."""
        proc = _start_server()
        try:
            output = _stop_server(proc)
            assert "threads/pool=" in output
            idx = output.index("threads/pool=") + len("threads/pool=")
            num_str = ""
            while idx < len(output) and output[idx].isdigit():
                num_str += output[idx]
                idx += 1
            assert int(num_str) >= 1
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()


class TestDuplicateServerDetection:
    """Test that launching a second server is rejected with a clear message."""

    def test_second_server_rejected(self):
        """A second server instance should fail with exit code 1."""
        proc1 = _start_server("-t", "1")
        try:
            proc2 = subprocess.Popen(
                [SERVER_BIN, "-t", "1"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=BUILD_DIR,
            )
            _, stderr = proc2.communicate(timeout=5)
            assert proc2.returncode == 1
            assert "already running" in stderr.decode()
        finally:
            if proc1.poll() is None:
                _stop_server(proc1)
            _cleanup_ipc()

    def test_server_starts_after_first_stops(self):
        """After the first server shuts down, a new one should start fine."""
        proc1 = _start_server("-t", "1")
        _stop_server(proc1)
        _cleanup_ipc()

        proc2 = _start_server("-t", "1")
        try:
            output = _stop_server(proc2)
            assert "Server started" in output
        finally:
            if proc2.poll() is None:
                proc2.kill()
                proc2.wait()
            _cleanup_ipc()


class TestShutdownModes:
    """Test --shutdown=drain and --shutdown=immediate flags."""

    def test_shutdown_drain_banner(self):
        """Server started with --shutdown=drain should show it in the banner."""
        proc = _start_server("-t", "1", "--shutdown=drain")
        try:
            output = _stop_server(proc)
            assert "shutdown=drain" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_shutdown_immediate_banner(self):
        """Server started with --shutdown=immediate should show it in the banner."""
        proc = _start_server("-t", "1", "--shutdown=immediate")
        try:
            output = _stop_server(proc)
            assert "shutdown=immediate" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_drain_shutdown_message(self):
        """Drain mode shutdown should report pending task count."""
        proc = _start_server("-t", "1", "--shutdown=drain")
        try:
            output = _stop_server(proc)
            assert "drain mode" in output.lower()
            assert "pending task(s) will be finished" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_immediate_shutdown_message(self):
        """Immediate mode shutdown should report discarding."""
        proc = _start_server("-t", "1", "--shutdown=immediate")
        try:
            output = _stop_server(proc)
            assert "immediate mode" in output.lower()
            assert "Discarding" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_default_is_drain(self):
        """Without --shutdown flag, default should be drain."""
        proc = _start_server("-t", "1")
        try:
            output = _stop_server(proc)
            assert "shutdown=drain" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()

    def test_invalid_shutdown_mode(self):
        """Unknown shutdown mode should fail with exit code 1."""
        _cleanup_ipc()
        proc = subprocess.Popen(
            [SERVER_BIN, "--shutdown=bogus"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=BUILD_DIR,
        )
        _, stderr = proc.communicate(timeout=5)
        assert proc.returncode == 1
        assert "Unknown shutdown mode" in stderr.decode()
        _cleanup_ipc()


class TestStatusReport:
    """Test SIGUSR1 status report output."""

    def test_sigusr1_prints_status(self):
        """Sending SIGUSR1 should produce a [STATUS] block on stdout."""
        proc = _start_server("-t", "1", "--shutdown=drain")
        try:
            proc.send_signal(signal.SIGUSR1)
            time.sleep(0.5)
            output = _stop_server(proc)
            assert "[STATUS] PID=" in output
            assert "math_pool:" in output
            assert "string_pool:" in output
            assert "slots:" in output
            assert "uptime=" in output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            _cleanup_ipc()


class TestSlotExhaustion:
    """Test behavior when all shared-memory slots are occupied."""

    def test_async_submit_fails_when_slots_full(self, capfd):
        """17th async request should fail when 16 slots are already occupied."""
        proc = _start_server("-t", "2", "--shutdown=drain")
        lib = _load_ipc_lib()
        try:
            assert lib.ipc_init() == 0

            for _ in range(IPC_MAX_SLOTS):
                req_id = ctypes.c_uint64()
                rc = lib.ipc_concat(b"a", b"b", ctypes.byref(req_id))
                assert rc == 0

            extra_id = ctypes.c_uint64()
            rc = lib.ipc_concat(b"x", b"y", ctypes.byref(extra_id))
            assert rc == -1

            _, err = capfd.readouterr()
            assert "no free slots" in err.lower()
        finally:
            lib.ipc_cleanup()
            if proc.poll() is None:
                _stop_server(proc)
            _cleanup_ipc()

    def test_sync_submit_fails_when_slots_full(self, capfd):
        """A blocking request should fail immediately if no slot is available."""
        proc = _start_server("-t", "2", "--shutdown=drain")
        lib = _load_ipc_lib()
        try:
            assert lib.ipc_init() == 0

            for _ in range(IPC_MAX_SLOTS):
                req_id = ctypes.c_uint64()
                rc = lib.ipc_concat(b"a", b"b", ctypes.byref(req_id))
                assert rc == 0

            out = ctypes.c_int32()
            rc = lib.ipc_add(1, 2, ctypes.byref(out))
            assert rc == -1

            _, err = capfd.readouterr()
            assert "no free slots" in err.lower()
        finally:
            lib.ipc_cleanup()
            if proc.poll() is None:
                _stop_server(proc)
            _cleanup_ipc()
