Runtime Constraints
===================

These constraints are part of the intended behavior and are mirrored from
``README.md`` to reduce drift:

- Numeric CLI inputs are integer-only (``int32_t``).
- Floating-point input (for example ``12.3``) is rejected at the client menu.
- String inputs for concat/search must be 1..16 characters each.
- Concat output capacity is 32 characters plus null terminator.
- Async requests are retrieved explicitly with the pending-results command.
- Maximum in-flight requests is 16 slots.
- Divide-by-zero is surfaced as a status error, not a numeric value.

Testing constraints:

- Run test suites without an external manually started server process.
- Test harness uses a global lock to avoid parallel suite interference.
