## Add accounts with hashed, peppered passwords

**Added**
- The Login server can create accounts and check logins, with names that ignore upper and lower case.
- Passwords are stored only as strong salted hashes, so a stolen database alone does not reveal them.
- The server refuses to start, with a clear message, if its secret pepper key is missing or replaced, instead of rejecting every login.
- Every login attempt is recorded.

Summary: The Login server now keeps accounts with securely stored passwords and protects the secret key they depend on (2026-10-26)
