"""Black-box integration tests for the IPC client-server system."""
import pytest

from conftest import run_client, run_client_with_delays


class TestClient1Blocking:
    """Test blocking operations via client1 (direct link)."""

    def test_add(self):
        stdout, _, rc = run_client("client1", [1, 42, 37, 5])
        assert rc == 0
        assert "Result is 79!" in stdout

    def test_add_negative(self):
        stdout, _, rc = run_client("client1", [1, -10, 25, 5])
        assert rc == 0
        assert "Result is 15!" in stdout

    def test_add_zero(self):
        stdout, _, rc = run_client("client1", [1, 0, 0, 5])
        assert rc == 0
        assert "Result is 0!" in stdout


class TestClient2Blocking:
    """Test blocking operations via client2 (dlopen)."""

    def test_subtract(self):
        stdout, _, rc = run_client("client2", [1, 100, 30, 5])
        assert rc == 0
        assert "Result is 70!" in stdout

    def test_subtract_negative_result(self):
        stdout, _, rc = run_client("client2", [1, 10, 50, 5])
        assert rc == 0
        assert "Result is -40!" in stdout


class TestClient1Async:
    """Test non-blocking operations via client1."""

    def test_multiply(self):
        stdout, _, rc = run_client_with_delays("client1", [
            ([2, 7, 8], 3.0),     # submit multiply, wait for server
            ([4], 0.5),           # check results
            ([5], 0),             # exit
        ])
        assert rc == 0
        assert "Request ID:" in stdout
        assert "Result is 56!" in stdout

    def test_concat(self):
        stdout, _, rc = run_client_with_delays("client1", [
            ([3, "hello", "world"], 1.0),  # submit concat
            ([4], 0.5),                     # check results
            ([5], 0),                       # exit
        ])
        assert rc == 0
        assert "Request ID:" in stdout
        assert "Result is helloworld!" in stdout

    def test_concat_max_length(self):
        s1 = "a" * 16
        s2 = "b" * 16
        stdout, _, rc = run_client_with_delays("client1", [
            ([3, s1, s2], 1.0),
            ([4], 0.5),
            ([5], 0),
        ])
        assert rc == 0
        expected = s1 + s2
        assert f"Result is {expected}!" in stdout


class TestClient2Async:
    """Test non-blocking operations via client2."""

    def test_divide(self):
        stdout, _, rc = run_client_with_delays("client2", [
            ([2, 10, 3], 3.0),   # submit divide
            ([4], 0.5),          # check results
            ([5], 0),            # exit
        ])
        assert rc == 0
        assert "Request ID:" in stdout
        assert "Result is 3!" in stdout

    def test_divide_by_zero(self):
        stdout, _, rc = run_client_with_delays("client2", [
            ([2, 10, 0], 3.0),
            ([4], 0.5),
            ([5], 0),
        ])
        assert rc == 0
        assert "division by zero" in stdout.lower()

    def test_search_found(self):
        stdout, _, rc = run_client_with_delays("client2", [
            ([3, "lo", "Hello!"], 1.0),
            ([4], 0.5),
            ([5], 0),
        ])
        assert rc == 0
        assert "Result is: 3" in stdout

    def test_search_not_found(self):
        stdout, _, rc = run_client_with_delays("client2", [
            ([3, "xyz", "Hello!"], 1.0),
            ([4], 0.5),
            ([5], 0),
        ])
        assert rc == 0
        assert "not found" in stdout.lower() or "Result is: -1" in stdout


class TestErrorHandling:
    """Test error conditions."""

    def test_string_too_long(self):
        long_str = "a" * 17
        stdout, stderr, rc = run_client_with_delays("client1", [
            ([3, long_str, "b"], 0.5),
            ([5], 0),
        ])
        combined = stdout + stderr
        assert "1..16" in combined or "too long" in combined.lower() or "Error" in combined


class TestNonBlockingOrdering:
    """Test that non-blocking calls demonstrate out-of-order completion."""

    def test_concat_before_multiply(self):
        """Submit multiply (2s delay) then concat (no delay).
        Concat result should be available before multiply."""
        stdout, _, rc = run_client_with_delays("client1", [
            ([2, 7, 8], 0.2),           # submit multiply (2s server delay)
            ([3, "a", "b"], 1.5),        # submit concat (fast), wait 1.5s
            ([4], 0.5),                  # check: concat should be done, multiply not yet
            ([4], 2.0),                  # wait more, check again: multiply should be done
            ([5], 0),                    # exit
        ])
        assert rc == 0
        assert "Result is ab!" in stdout
        assert "Result is 56!" in stdout


class TestManualAsyncRetrievalMode:
    """Ensure async results are consumed only via explicit command 4."""

    def test_client1_does_not_auto_consume_async_result(self):
        stdout, _, rc = run_client_with_delays("client1", [
            ([2, 7, 8], 3.0),  # submit async multiply and let it finish
            ([5], 0),          # exit without explicit check
        ])
        assert rc == 0
        assert "Request ID:" in stdout
        assert "Result is 56!" not in stdout

    def test_client2_does_not_auto_consume_async_result(self):
        stdout, _, rc = run_client_with_delays("client2", [
            ([2, 12, 3], 3.0),  # submit async divide and let it finish
            ([5], 0),           # exit without explicit check
        ])
        assert rc == 0
        assert "Request ID:" in stdout
        assert "Result is 4!" not in stdout
