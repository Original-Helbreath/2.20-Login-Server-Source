## Add every Login packet as a typed struct

### Login Server

**Added**
- The `message_id` enum covering every client <-> Login and Login <-> World message (0x01xx to 0x04xx), with `direction_of()` and `is_known()`.
- Every packet of the new Login protocol (plan sect. 4.4 and 4.5) as a typed struct in `shared/protocol` (`login_messages.h`, `link_messages.h`), with size and per-field offset asserts.
- Received packets are refused when an enum is out of range, a flag is not 0 or 1, or a text field carries bytes after its terminator (`field_checks.h`, `is_well_formed`).
- Typed encode and decode in `message_codec.h`, including character lists, so each message travels under its own id; the encoder refuses exactly what the decoder refuses, and a refused list leaves nothing behind.
- 74 message tests in `login_server_tests` (`message_tests.cpp`), bringing it to 179 checks.

Summary: Login Server gains the shared, typed client <-> Login and Login <-> World packet structs with their codec and receipt checks (2026-10-27)
