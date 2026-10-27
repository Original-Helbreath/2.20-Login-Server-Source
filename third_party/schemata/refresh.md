# Refreshing the vendored Schemata archives

Schemata is the user's own C++20 code-first ORM, in its own repository, built with CMake. This solution
builds with Visual Studio projects, so Schemata is vendored as prebuilt static archives plus headers, the
same pattern Helbreath-SDL uses (`external/schemata/refresh.md` there). Nothing here is edited by hand.
A fault in Schemata is reported to the Schemata session, never patched here (og-helbreath rule).

## What is pinned

| | |
|---|---|
| Source | `https://github.com/shadowevil/schemata` (locally `source/entityframeworkcpp`) |
| Tag | `v0.1.0-rc.24` |
| Commit | `ea70255a28e2700928531eeb2bcfd4caab05e488` |
| Vendored | 2026-10-26 (LOGIN-002) |
| Backends | `sch_sqlite` only, with the SQLite amalgamation compiled in (`sqlite3_amalgamation`) |
| SQLite | 3.53.4 amalgamation; `sqlite3.c` sha256 `b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189`, `sqlite3.h` sha256 `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` |
| Built with | MSVC 19.51.36260.0, generator "Visual Studio 18 2026", x64, `/MD` and `/MDd` (`SCHEMATA_STATIC_RUNTIME` off) |

Configure printed `sch_sqlite : enabled (amalgamation)`, `sch_mysql : disabled`, `sch_mariadb : disabled`.

| file | sha256 |
|---|---|
| `lib/x64/debug/sch_cored.lib` | `bda039998f5e56e605bd7080a551760cc8480fe39500197119cff9367c0fd0be` |
| `lib/x64/debug/sch_cored.pdb` | `5eecbbd38e4c7ed562da8bbc4a0aec74124368028c3e65040fbf39866ebe595d` |
| `lib/x64/debug/sch_sqlited.lib` | `d043fd7f535528db51e325b14a3272ecb8517be79f28f1fd52ebc38737690bd5` |
| `lib/x64/debug/sch_sqlited.pdb` | `f1c591280f01bd49e11e842c5dde2884b5f30cba8d11a8f59a2f5c457273ab98` |
| `lib/x64/debug/sqlite3_amalgamationd.lib` | `4e6c4486da329c1e15692f87b230d5c4a7fc3eda98b09479a55653e05f4dcbcb` |
| `lib/x64/debug/sqlite3_amalgamationd.pdb` | `a6e27c1cb8bb417474c7a27dcbd79f1b0da65c398d012eb9e4162fb905ce81d8` |
| `lib/x64/release/sch_core.lib` | `c89548c70331a37e7d1b30d0075960798601d8343031b0a1b487c4a8bc08cc78` |
| `lib/x64/release/sch_core.pdb` | `9d49983ea72f52f7eddc0c6660465fe7cb5376cb50ad15ff5aa2a4595ebffd04` |
| `lib/x64/release/sch_sqlite.lib` | `55764f70e0fc8b6700ef54aeac0836d224124d3b82f75caf98532eae44dbe006` |
| `lib/x64/release/sch_sqlite.pdb` | `a72ff968a50517b2e6972c25d5cc3724bc4c37eea59a7c56cd89b97e664420f5` |
| `lib/x64/release/sqlite3_amalgamation.lib` | `35e6a1bb07d217e93dd2c9bc3c546424969b99efe283e5024a59be57fac5b461` (no PDB is produced for it) |

## How they were built

Export the tag, never the moving checkout, into a short scratch path inside the og-helbreath workspace
(long paths break MSBuild's FileTracker; system temp is off limits). Confirm the tag in Git Bash:

```bash
git -C <schemata> rev-parse "v0.1.0-rc.24^{commit}"   # expect ea70255a28e2700928531eeb2bcfd4caab05e488
mkdir -p <scratch>/sv24/src <scratch>/sv24/sqlite
git -C <schemata> archive "v0.1.0-rc.24" | tar -x -m -C <scratch>/sv24/src
cp <sqlite amalgamation>/sqlite3.c <sqlite amalgamation>/sqlite3.h <scratch>/sv24/sqlite/
```

Then, from PowerShell with CMake on PATH (Visual Studio ships one):

```powershell
$env:MSBUILDDISABLENODEREUSE = '1'
cmake -S "$sv\src" -B "$sv\b" -G "Visual Studio 18 2026" -A x64 `
	-D SCHEMATA_BUILD_SQLITE=ON -D SCHEMATA_BUILD_MYSQL=OFF -D SCHEMATA_BUILD_MARIADB=OFF `
	-D SCHEMATA_BUILD_TESTS=OFF -D SCHEMATA_BUILD_SCAFFOLD=OFF -D SCHEMATA_BUILD_EXAMPLES=OFF `
	-D SCHEMATA_SQLITE_AMALGAMATION="$sv\sqlite"
cmake --build "$sv\b" --config Debug   --target sch_sqlite -- /nodeReuse:false /m
cmake --build "$sv\b" --config Release --target sch_sqlite -- /nodeReuse:false /m
```

## What is copied in

```
<src>/include/schemata/**                 -> include/schemata/   (copied with tar -m: fresh timestamps)
<b>/lib/Debug/sch_cored.{lib,pdb}         -> lib/x64/debug/
<b>/lib/Debug/sch_sqlited.{lib,pdb}       -> lib/x64/debug/
<b>/lib/Debug/sqlite3_amalgamationd.{lib,pdb} -> lib/x64/debug/
<b>/lib/Release/sch_core.{lib,pdb}        -> lib/x64/release/
<b>/lib/Release/sch_sqlite.{lib,pdb}      -> lib/x64/release/
<b>/lib/Release/sqlite3_amalgamation.lib  -> lib/x64/release/
```

On a refresh, compare the header lists first: a header upstream removed must be deleted here by literal path.
Update the pin and both hash tables in the same change set, and rebuild everything that links Schemata.
