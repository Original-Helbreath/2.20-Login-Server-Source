## Broker characters through the World

### Login Server

**Added**
- After login the Login server relays the character list from the World, sending `login_response` ok only together with the list.
- Create and delete character requests are format-checked (M13), relayed to the World with request ids and timeouts, and answered with the World's result and a fresh list.
- `login_request.world` is checked against the configured worlds (`no_such_world`); a World that is not linked or does not answer in time gives `world_unavailable`.
- Character tests against a scripted World stub, bringing `login_server_tests` to 391 checks.

Summary: Login Server brokers character list, create and delete between the client and the World (2026-10-27)
