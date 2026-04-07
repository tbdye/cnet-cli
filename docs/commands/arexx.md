# ARexx Commands

ARexx commands send ARexx IPC messages to CNet BBS processes and return the response. Two subcommands target different ARexx ports: `send` targets per-port CNETREXX ports, while `control` targets the system-wide CONTROLREXX.1 port.

All ARexx commands require `rexxsyslib.library` to be available on the Amiga. The library is opened at startup; if it is missing, ARexx commands return an error but cnet-cli continues to function for non-ARexx commands.

---

## `cnet-cli arexx send`

### Synopsis
```
cnet-cli arexx send <port-number> <command...>
```

### Description
Sends an ARexx command string to the per-port ARexx port `CNETREXX{N}`, where `{N}` is the port number. The command blocks until the target port processes the message and sends a reply. Returns the ARexx return code and any result string.

Each loaded CNet port registers an ARexx port named `CNETREXX0`, `CNETREXX1`, etc. These ports accept CNet's ARexx command vocabulary (e.g., `DROPCARRIER`, `GETVAR`, `PUTVAR`). The port must be loaded and running for the message to be delivered.

This command can perform both read and write operations depending on the ARexx command sent.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port-number>` | Yes | CNet port number (0-99). Must be all digits. Determines the target ARexx port name: `CNETREXX{N}` |
| `<command...>` | Yes | One or more arguments concatenated with spaces to form the ARexx command string. Maximum 511 characters after concatenation |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `port` | string | The ARexx port name that was targeted (e.g., `"CNETREXX0"`) |
| `command` | string | The full command string that was sent |
| `rc` | number | ARexx return code from the target (0 typically indicates success) |
| `result` | string or null | Result string returned by the target, or `null` if no result string was provided |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"Cannot open rexxsyslib.library"` -- library not available
- `"Port number must be 0-99"` -- invalid port number
- `"ARexx port CNETREXX{N} not found"` -- target port not loaded or not running

### Notes
- The port name format is `CNETREXX{N}` with no zero-padding and no dot separator (e.g., `CNETREXX0`, `CNETREXX12`, not `CNETREXX.0`).
- The command string buffer is 512 bytes. Arguments beyond that limit are silently truncated.
- The result string buffer is 1024 bytes. Responses longer than that are truncated.
- Result strings longer than 4096 bytes from the ARexx reply are discarded (treated as corrupt) and `result` will be `null`.
- The command blocks until the target port replies. If the target port hangs, cnet-cli will hang indefinitely.
- Requires `rexxsyslib.library` (opened at cnet-cli startup).

---

## `cnet-cli arexx control`

### Synopsis
```
cnet-cli arexx control <command...>
```

### Description
Sends an ARexx command string to the CNet Control panel's ARexx port `CONTROLREXX.1`. This is the system-wide control port that accepts administrative commands like `RUNPORT` and `CLOSEPORT`. The command blocks until the control panel processes the message and sends a reply.

This command can perform both read and write operations depending on the ARexx command sent.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<command...>` | Yes | One or more arguments concatenated with spaces to form the ARexx command string. Maximum 511 characters after concatenation |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `port` | string | Always `"CONTROLREXX.1"` |
| `command` | string | The full command string that was sent |
| `rc` | number | ARexx return code from the control panel (0 typically indicates success) |
| `result` | string or null | Result string returned by the control panel, or `null` if no result string was provided |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"Cannot open rexxsyslib.library"` -- library not available
- `"ARexx port CONTROLREXX.1 not found"` -- CNet Control panel not running

### Notes
- The target port is always `CONTROLREXX.1` (hardcoded). No port number argument is needed.
- The command string buffer is 512 bytes. Arguments beyond that limit are silently truncated.
- The result string buffer is 1024 bytes. Responses longer than that are truncated.
- The command blocks until the control panel replies.
- The `port load` and `port unload` commands are higher-level wrappers around `arexx control` that send `RUNPORT` and `CLOSEPORT` respectively. Use those for port management instead of calling `arexx control` directly.
- Requires `rexxsyslib.library` (opened at cnet-cli startup).
