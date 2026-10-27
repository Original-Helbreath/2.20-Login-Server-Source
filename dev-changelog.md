## Add the shared packet framing layer

### Login Server

**Added**
- The shared `hb::net` framing layer in `shared/protocol` (the 2.20-Shared submodule): an 8-byte `frame_header`, a stream `frame_decoder` and the encoders.
- `span_reader` and `span_writer`, which copy whole wire structs in and out of byte spans and enforce exact body sizes; `wire_type` rejects padded, bool and pointer types at compile time.
- `fixed_string<N>` and the plan sect. 4.3 shared field types, each with its size and field offsets checked at compile time.
- Frames are refused when they are oversize, carry the wrong protocol version, have a nonzero reserved byte, or are short or long.
- The frame decoder checks every header as soon as it arrives and accepts any number of complete frames in one read, holding at most one partial frame between reads.
- `max_body_bytes` is capped at 16 MiB everywhere so frame size sums cannot overflow on the 32-bit client.
- 58 protocol unit tests in `login_server_tests` (`protocol_tests.cpp`, shared `test_runner.h`), bringing it to 105 checks.

### Project

**Added**
- `shared/` git submodule pointing at Original-Helbreath/2.20-Shared (branch development); both projects include `$(SolutionDir)shared\protocol`.

Summary: Login Server gains the shared hb::net framing, codec and wire-type layer from the 2.20-Shared submodule, with 58 new tests (2026-10-26)
