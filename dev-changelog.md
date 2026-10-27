## Hand players off to the World

### Login Server

**Added**
- `enter_game_request` reserves the account, draws a 32-byte CSPRNG ticket, sends `session_grant` to the World and answers `enter_game_response` with the World's client address, port and the ticket, then closes the session.
- An in-memory account online registry (offline, handoff pending, in world): the ticket lifetime expires an unconfirmed handoff to offline, and the World's `session_started` and `session_ended` drive the rest.
- `already_playing` answers a normal enter for a pending or in-world account; `force_login` sends `kick_account` to the holding World and answers `force_disconnect_sent`.
- A World claiming an account another World holds is refused and sent `kick_account`; World link loss sets that World's accounts offline, and re-announced `session_started` messages rebuild the state.
- Enter-game and claim tests against the World stub bring `login_server_tests` to 511 checks.

Summary: Login Server hands logged-in players to the World with a one-time ticket and tracks who is online (2026-10-27)
