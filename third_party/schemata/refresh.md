# Refreshing the vendored Schemata archives

Schemata is the user's own C++20 code-first ORM, in its own repository, built with CMake. This solution
builds with Visual Studio projects, so Schemata is vendored as prebuilt static archives plus headers, the
same pattern Helbreath-SDL uses (`external/schemata/refresh.md` there). Nothing here is edited by hand.
A fault in Schemata is reported to the Schemata session, never patched here (og-helbreath rule).

## What is pinned

| | |
|---|---|
| Source | `https://github.com/shadowevil/schemata` (locally `source/entityframeworkcpp`) |
| Tag | `v0.1.0-rc.25` |
| Commit | `5d11dd7b1c57534c6997f7cf1e97f232784b9002` |
| Vendored | 2026-10-26 (LOGIN-002); refreshed to rc.25 2026-10-27 (WORLD-007, SCH-0054) |
| Backends | `sch_sqlite` only, with the SQLite amalgamation compiled in (`sqlite3_amalgamation`) |
| SQLite | 3.53.4 amalgamation; `sqlite3.c` sha256 `b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189`, `sqlite3.h` sha256 `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` |
| Built with | MSVC 19.51.36260.0, generator "Visual Studio 18 2026", x64, `/MD` and `/MDd` (`SCHEMATA_STATIC_RUNTIME` off) |

Configure printed `sch_sqlite : enabled (amalgamation)`, `sch_mysql : disabled`, `sch_mariadb : disabled`.

| file | sha256 |
|---|---|
| `lib/x64/debug/sch_cored.lib` | `af3f636492097dab0f87807a39b79a9ce80f7ab77c98b640f75f8c1f59cd6831` |
| `lib/x64/debug/sch_cored.pdb` | `8ec7dd708c6b863c7bd22056f606abcae14c1c19ede9c57d933233c909f4051c` |
| `lib/x64/debug/sch_sqlited.lib` | `696941a210383e53bf25d088e4bd70413412e27b3b2635593fc8d4f166e14d24` |
| `lib/x64/debug/sch_sqlited.pdb` | `47d8d785749b73661e351dd4d08c831ae0582000fc50b89d7d10f0ad26405826` |
| `lib/x64/debug/sqlite3_amalgamationd.lib` | `071b15742fe1aba0ed074935bfeb0613734c071dda212e9032afed90ff240d4a` |
| `lib/x64/debug/sqlite3_amalgamationd.pdb` | `441d0409cf2491c2d06019edb51faed8c35da97387566280ba28cda1cdefae35` |
| `lib/x64/release/sch_core.lib` | `43e4238e04e4c68baffc1f0f267b39ffb76e9e96be7fd8a623d1ea4becf155dd` |
| `lib/x64/release/sch_core.pdb` | `d847d36f8ffb8c7ec3097796c6ccb7b12793f192b273b64586221739d5bfa955` |
| `lib/x64/release/sch_sqlite.lib` | `8ccce4bd0ca5d7fba6c9381e7a4610d8f14191e23c34debb60fb865ebb2ea3eb` |
| `lib/x64/release/sch_sqlite.pdb` | `576723cda52346694ec0a051fc19ed0b6171afe3193ff0bb88a9dcd06686f5da` |
| `lib/x64/release/sqlite3_amalgamation.lib` | `4a71bf470298406bc0776e0b3b4985010c8e3a67e959ea6d7b1a4b1073710635` (no PDB is produced for it) |

## How they were built

Export the tag, never the moving checkout, into a short scratch path inside the og-helbreath workspace
(long paths break MSBuild's FileTracker; system temp is off limits). Confirm the tag in Git Bash:

```bash
git -C <schemata> rev-parse "v0.1.0-rc.25^{commit}"   # expect 5d11dd7b1c57534c6997f7cf1e97f232784b9002
mkdir -p <scratch>/sv25/src <scratch>/sv25/sqlite
git -C <schemata> archive "v0.1.0-rc.25" | tar -x -m -C <scratch>/sv25/src
cp <sqlite amalgamation>/sqlite3.c <sqlite amalgamation>/sqlite3.h <scratch>/sv25/sqlite/
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
