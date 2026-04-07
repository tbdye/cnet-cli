# cnet-cli

Standalone 68k AmigaOS CLI for CNet BBS system administration and content management.

## Overview

cnet-cli provides programmatic access to a running CNet 5.x BBS from outside the BBS environment. It attaches directly to CNet's MainPort with full sysop privileges, bypassing the normal login flow. No BBS port is consumed and no user session is created. The caller's identity is a parameter, so it can operate as any user account.

All output is structured JSON, making cnet-cli suitable as a backend for automation, monitoring, and remote administration tooling. It is designed to be invoked from a Linux host via [amigactl](https://github.com/tbdye/amigactl) remote execution, though it can also be run directly from an AmigaOS shell.

The binary cross-compiles with m68k-amigaos-gcc, links against [cnet-sdk](https://github.com/tbdye/cnet-sdk) headers at compile time, and opens cnet.library at runtime. Compile-time static assertions verify that all struct sizes match the live SAS/C ABI.

## Command Groups

cnet-cli organizes 89 commands into 22 top-level entry points across 19 functional areas.

| Group | Commands | Description |
|-------|----------|-------------|
| `status` | 1 | System name, version, serial, registration, totals |
| `ports` | 1 | All port status and activity |
| `who` | 1 | Online users (summary, detail, per-port) |
| `sub` | 8 | Subboard CRUD, tree traversal, disk usage |
| `msg` | 8 | Message list, read, post, respond, edit, delete, search, move |
| `user` | 9 | User accounts: list, show, find, plan, edit, enable/disable, profile, delete |
| `mail` | 12 | Mail send/read/reply/delete, folders, feedback, verify, alias management |
| `file` | 8 | File area catalog: list, show, add, edit, remove, validate, find, missing detection |
| `news` | 5 | News/text items: list, read, post, edit, delete |
| `gfile` | 4 | GFile items: list, read, add, remove |
| `olm` | 1 | Send Online Messages (with broadcast support) |
| `group` | 4 | Access groups: list, show, edit, transpose privileges |
| `config` | 4 | BBS configuration, control panel flags, text reload, per-port config |
| `log` | 4 | Log files: list, read, callers log, structured callers parser |
| `stats` | 1 | System statistics |
| `arexx` | 2 | ARexx IPC to port and control channels |
| `port` | 3 | Port management: load, unload, dump |
| `conf` | 1 | Conference room listing |
| `event` | 2 | Scheduled event listing and detail |
| `maint` | 4 | Maintenance: pointer rebuild, counter recount, mail/sub repair |
| `bbslist` | 1 | BBS directory listing |
| `vote` | 3 | Voting booth: list topics, show detail, view results |

See [docs/commands/](docs/commands/) for the full command reference.

## Building

### Prerequisites

- **m68k-amigaos-gcc** cross-compiler (installed at `/opt/amiga/bin/`)
- **cnet-sdk** headers -- clone [tbdye/cnet-sdk](https://github.com/tbdye/cnet-sdk) alongside this repo

### Build

```
make CNET_SDK_PATH=../cnet-sdk
```

If `cnet-sdk` is checked out as a sibling directory, the default path applies and `make` alone is sufficient.

Key compiler flags: `-noixemul -m68020 -O2 -Wall -Wextra -Werror`. The binary targets 68020+ processors and uses the libnix C library (no ixemul.library dependency).

The resulting binary is approximately 223 KB.

## Deployment

Deploy the binary to the Amiga via amigactl and invoke commands remotely:

```
amigactl put cnet-cli "C:cnet-cli"
amigactl exec "C:cnet-cli status"
amigactl exec "C:cnet-cli sub list --active"
amigactl exec "C:cnet-cli user show sysop"
```

cnet-cli can also be run directly from an AmigaOS CLI shell:

```
C:cnet-cli status
C:cnet-cli msg list General --limit 10
```

## Runtime Dependencies

cnet-cli requires a running CNet 5.x BBS instance on the same Amiga.

| Library | Required | Purpose |
|---------|----------|---------|
| `cnet.library` | Yes | Core BBS data access (MainPort, subboards, users, items) |
| `cnetmail.library` | No | Mail send and reply operations |
| `cnet4.library` | No | Timestamp conversion and range parsing |
| `rexxsyslib.library` | No | ARexx IPC for `arexx` and `port` commands |

Optional libraries degrade gracefully. If an optional library is unavailable, commands that require it report an error; all other commands work normally.

## Testing

Integration tests run against a live CNet BBS via amigactl:

```
python3 tests/test_integration.py --host 192.168.6.228
```

To skip mutation tests (create, edit, delete) and run only read-only validation:

```
python3 tests/test_integration.py --host 192.168.6.228 --skip-mutations
```

Tests create temporary data prefixed with `_test_` and clean up after themselves.
