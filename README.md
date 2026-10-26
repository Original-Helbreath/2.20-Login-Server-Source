# 2.20-Login-Server-Source

Source for the Login server: account authentication, the character-selection screen and the handoff to the World server. It reads no game files and saves no characters in play..

## Contents

Part of the two-binary recreation of the 2.20 servers (Login + World), written new in C++20 to the org
[Standards](https://github.com/Original-Helbreath/Standards). It replaces `2.20-Main-Log-Server-*` and `2.20-World-Log-Server-*`, which are kept as found in
[2.20-Reference](https://github.com/Original-Helbreath/2.20-Reference) (`as-found/servers/`). Empty until the
first ticket lands; the plan and its tickets are on the project board.

## Branches

| Branch | Purpose |
|---|---|
| `development` | Integration branch. Work happens in feature branches named for their ticket, which merge into `development`. |
| `stable` | Default branch. `development` is merged here once it is stable. |

This repo has no `original` branch: it holds new code, not files transferred from elsewhere.

## Project

Part of [Original Helbreath](https://github.com/Original-Helbreath): keeping the original 1999-2005 Siementech Helbreath alive.
Repos are named `<version>-<Part>`; other versions follow the same pattern.
Issues are tracked on the [project board](https://github.com/orgs/Original-Helbreath/projects/1). The analysis behind everything is in
[2.20-Documentation](https://github.com/Original-Helbreath/2.20-Documentation).
