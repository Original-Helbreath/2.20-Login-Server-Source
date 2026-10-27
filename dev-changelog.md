## Authenticate clients

### Login Server

**Added**
- Client authentication: `login_request`, `create_account_request` and `change_password_request` are handled, with hashing and database work on the worker pool.
- A bad password and an unknown account get the same reply, and a locked account is reported only after the correct password.
- A second login for an account closes its first session; login clears the pre-login deadline and applies the after-login idle timeout.
- Account creation (when enabled), password changes and every login attempt are recorded in `login_events`, including creations refused as disabled or over the cap.
- A connection is closed after three failed logins or password changes (including an unusable stored password) or four account-creation requests.
- 78 new checks (`auth_tests.cpp`), bringing `login_server_tests` to 313.

Summary: Login Server authenticates clients, creates accounts and changes passwords with audit and per-connection limits (2026-10-27)
