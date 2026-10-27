## Listen for clients and World servers

### Login Server

**Added**
- The Login server listens for game clients and World servers on the configured ports and shuts down cleanly on Ctrl+C or SIGTERM.
- A World server registers only from its allow-listed address and with its link key; a wrong key, an unknown name or a duplicate is refused, and a stranger gets no reply.
- The World link exchanges heartbeats both ways and is marked lost after three missed intervals.
- Client connections close on a malformed frame, an unexpected message id or the pre-login deadline; a World must register within 3 x the heartbeat.
- Each listener caps its open connections (`listen.max_client_connections`, `listen.max_world_connections`).
- Asio 1.36.0, the logger and a shared network layer (`hb::net::server`, `hb::net::client`, `hb::net::connection`) now live in 2.20-Shared for the World server and client to build on; `hb::log::write(level, format, args...)` added.
- 56 network checks, bringing `login_server_tests` to 235.

**Fixed**
- Closing a connection while a write was in flight dropped that write's buffer and crashed the Debug tests.
- Test runs report CRT asserts on stderr instead of opening a dialog.

Summary: Login Server accepts game clients and authenticated World links over the new shared Asio network layer (2026-10-27)
