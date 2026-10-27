## Add every Login packet as a typed struct

**Added**
- Every message between the client, the Login server and the World server now has one fixed, shared layout.
- Malformed messages are refused instead of being half-read.

Summary: The Login protocol's messages are defined once and shared by every program that sends them (2026-10-27)
