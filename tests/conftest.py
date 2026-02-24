"""Pytest fixtures for IPC client-server integration tests."""
import errno
import fcntl
import os
import signal
import subprocess
import time
from typing import List

import pytest

BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "build")
SERVER_BIN = os.path.join(BUILD_DIR, "server")
SERVER_BIN_REALPATH = os.path.realpath(SERVER_BIN)
SHM_PATH = "/dev/shm/ipc_shm"
PYTEST_LOCK_FILE = "/tmp/ipc_pytest.lock"


def _pid_is_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _wait_pids_exit(pids: List[int], timeout_sec: float) -> List[int]:
    deadline = time.time() + timeout_sec
    remaining = list(dict.fromkeys(pids))
    while remaining and time.time() < deadline:
        remaining = [pid for pid in remaining if _pid_is_alive(pid)]
        if remaining:
            time.sleep(0.05)
    return remaining


def list_workspace_server_pids() -> List[int]:
    """List running PIDs whose executable matches this workspace server binary."""
    pids = []
    try:
        proc_entries = os.listdir("/proc")
    except Exception:
        return []

    for name in proc_entries:
        if not name.isdigit():
            continue
        pid = int(name)
        if pid == os.getpid():
            continue
        try:
            exe_path = os.path.realpath(f"/proc/{pid}/exe")
        except OSError:
            continue
        if exe_path == SERVER_BIN_REALPATH:
            pids.append(pid)

    return sorted(set(pids))


def ensure_no_external_server_running(context: str, allowed_pids=None):
    """Fail fast if a server process exists outside allowed ownership."""
    allowed = set(allowed_pids or [])
    pids = [pid for pid in list_workspace_server_pids() if pid not in allowed]
    if pids:
        raise RuntimeError(
            f"{context}: external server process(es) detected: {pids}. "
            "Stop manual server instances before running pytest."
        )


def cleanup_orphan_servers():
    """Terminate orphan server processes from this workspace build path."""
    pids = list_workspace_server_pids()
    if not pids:
        return

    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    remaining = _wait_pids_exit(pids, timeout_sec=1.5)
    for pid in remaining:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    _wait_pids_exit(remaining, timeout_sec=1.0)


def acquire_test_run_lock_nonblocking(lock_path: str = PYTEST_LOCK_FILE) -> int:
    """Acquire a process-wide pytest lock to avoid concurrent IPC test runs."""
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        os.close(fd)
        if exc.errno in (errno.EACCES, errno.EAGAIN):
            raise RuntimeError(
                f"Another IPC pytest run is active (lock: {lock_path}). "
                "Run test suites sequentially."
            ) from exc
        raise
    return fd


def release_test_run_lock(fd: int):
    try:
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def try_acquire_lock_for_tests(lock_path: str = PYTEST_LOCK_FILE):
    """Internal helper used by tests to validate lock contention behavior."""
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        os.close(fd)
        if exc.errno in (errno.EACCES, errno.EAGAIN):
            return None
        raise
    return fd


def cleanup_ipc_files():
    """Remove known IPC objects and lock file."""
    # Never touch global IPC objects while any server process is still running.
    if list_workspace_server_pids():
        return
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
    # Keep lock file path; flock lock ownership is inode-based and stale path is harmless.


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "self_managed_server: tests that manage their own server lifecycle",
    )


def pytest_collection_modifyitems(config, items):
    config._needs_session_server = any(  # type: ignore[attr-defined]
        "self_managed_server" not in item.keywords for item in items
    )


def pytest_sessionstart(session):
    """Startup diagnostics and fail-fast guard for external server detection."""
    pids = list_workspace_server_pids()
    if os.environ.get("IPC_TEST_DEBUG_PIDS", "1") != "0":
        print(f"[IPC_TEST_DEBUG] server_bin: {SERVER_BIN}")
        print(f"[IPC_TEST_DEBUG] server_bin_realpath: {SERVER_BIN_REALPATH}")
        print(f"[IPC_TEST_DEBUG] detected server pids at session start: {pids}")
    if pids and os.environ.get("IPC_TEST_ABORT_ON_EXTERNAL", "1") != "0":
        pytest.exit(
            "Aborting pytest: external server instance(s) detected at startup: "
            f"{pids}. Stop manual server processes before running tests.",
            returncode=2,
        )


@pytest.fixture(scope="session", autouse=True)
def ipc_test_run_lock():
    """Ensure only one IPC pytest invocation runs at a time."""
    fd = acquire_test_run_lock_nonblocking()
    try:
        yield
    finally:
        release_test_run_lock(fd)


@pytest.fixture(scope="session", autouse=True)
def server_process(request, ipc_test_run_lock):
    """Start the server before all tests and shut it down after."""
    # Skip global fixture for suites that self-manage server lifecycle.
    if not getattr(request.config, "_needs_session_server", True):
        yield None
        return

    ensure_no_external_server_running("server_process startup")
    cleanup_ipc_files()

    proc = subprocess.Popen(
        [SERVER_BIN, "-t", "2"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=BUILD_DIR,
        start_new_session=True,
    )

    try:
        # Wait for shared memory to appear (server is ready)
        for _ in range(50):
            if os.path.exists(SHM_PATH):
                break
            time.sleep(0.1)
        else:
            proc.kill()
            raise RuntimeError("Server did not create shared memory in time")

        # Small extra delay for semaphore initialization
        time.sleep(0.2)

        yield proc
    finally:
        # Graceful shutdown of fixture-owned process only.
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGINT)
            except ProcessLookupError:
                pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                proc.wait()

        ensure_no_external_server_running("server_process teardown", allowed_pids={proc.pid})
        cleanup_ipc_files()


def run_client(client_name, inputs, timeout=10):
    """Run a client binary with scripted stdin and return stdout."""
    client_bin = os.path.join(BUILD_DIR, client_name)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = BUILD_DIR

    proc = subprocess.Popen(
        [client_bin],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=BUILD_DIR,
        env=env,
    )

    stdin_data = "\n".join(str(i) for i in inputs) + "\n"
    stdout, stderr = proc.communicate(input=stdin_data.encode(), timeout=timeout)
    return stdout.decode(), stderr.decode(), proc.returncode


def run_client_with_delays(client_name, input_groups, timeout=15):
    """Run a client with timed input groups for async testing.

    input_groups is a list of (inputs_list, delay_after_seconds) tuples.
    """
    client_bin = os.path.join(BUILD_DIR, client_name)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = BUILD_DIR

    proc = subprocess.Popen(
        [client_bin],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=BUILD_DIR,
        env=env,
    )

    for inputs, delay in input_groups:
        if proc.poll() is not None:
            break
        data = "\n".join(str(i) for i in inputs) + "\n"
        try:
            proc.stdin.write(data.encode())
            proc.stdin.flush()
        except (BrokenPipeError, ValueError):
            # Client exited early; collect whatever output is available.
            break
        if delay > 0:
            time.sleep(delay)

    # Python 3.13+ may flush stdin inside communicate(); avoid "flush of closed file"
    # after we intentionally close stdin for interactive client scripts.
    if proc.stdin is not None:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.stdin = None
    stdout, stderr = proc.communicate(timeout=timeout)
    return stdout.decode(), stderr.decode(), proc.returncode
