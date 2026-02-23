"""Pytest fixtures for IPC client-server integration tests."""
import os
import signal
import subprocess
import time

import pytest

BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "build")
SERVER_BIN = os.path.join(BUILD_DIR, "server")
SHM_PATH = "/dev/shm/ipc_shm"


@pytest.fixture(scope="session", autouse=True)
def server_process():
    """Start the server before all tests and shut it down after."""
    # Clean up leftover IPC objects and lock file from prior crashed runs
    if os.path.exists(SHM_PATH):
        os.remove(SHM_PATH)
    lock_file = "/tmp/ipc_server.lock"
    if os.path.exists(lock_file):
        os.remove(lock_file)

    proc = subprocess.Popen(
        [SERVER_BIN, "-t", "2"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=BUILD_DIR,
    )

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

    # Graceful shutdown
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


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
        data = "\n".join(str(i) for i in inputs) + "\n"
        proc.stdin.write(data.encode())
        proc.stdin.flush()
        if delay > 0:
            time.sleep(delay)

    proc.stdin.close()
    stdout, stderr = proc.communicate(timeout=timeout)
    return stdout.decode(), stderr.decode(), proc.returncode
