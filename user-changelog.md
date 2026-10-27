## Add the shared packet framing layer

**Added**
- The servers and client now share one definition of how network messages are packaged, so they cannot drift apart.
- Malformed or oversized network messages are refused before they can use up memory.

Summary: The groundwork for the new network protocol is in place, shared by every program that speaks it (2026-10-26)
