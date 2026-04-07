# Port Commands

Port commands manage CNet BBS port lifecycle and user sessions. All three subcommands are thin wrappers around ARexx IPC: `load` and `unload` send commands to the `CONTROLREXX.1` control port, while `dump` sends `DROPCARRIER` to the per-port `CNETREXX{N}` port.

All port commands require `rexxsyslib.library` to be available on the Amiga.

---

## `cnet-cli port load`

### Synopsis
```
cnet-cli port load <port-number>
```

### Description
Loads (starts) a CNet BBS port by sending the `RUNPORT <N>` ARexx command to the `CONTROLREXX.1` control port. This instructs the CNet Control panel to initialize and activate the specified port.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port-number>` | Yes | CNet port number to load (0-99). Must be all digits |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | The port number that was targeted |
| `action` | string | Always `"load"` |
| `rc` | number | ARexx return code from the control panel (0 typically indicates success) |
| `warning` | string | Always present: `"CONTROLREXX.1 commands may not take effect on CNet v5.36b"` |
| `warnings` | array | Additional warnings (present only if there are extra warnings beyond the fixed one) |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"Cannot open rexxsyslib.library"` -- library not available
- `"Port number must be 0-99"` -- invalid port number
- `"ARexx port CONTROLREXX.1 not found (CNet Control not running?)"` -- control panel not running

### Notes
- A warning is emitted if the port number exceeds the configured `HiPort` value from MainPort (the highest port number configured in CNet).
- On CNet v5.36b, `RUNPORT` returns `RC=0` but may have no observable effect. The fixed warning in the output documents this known issue.
- Requires `rexxsyslib.library` (opened at cnet-cli startup).

---

## `cnet-cli port unload`

### Synopsis
```
cnet-cli port unload <port-number>
```

### Description
Unloads (stops) a CNet BBS port by sending the `CLOSEPORT <N>` ARexx command to the `CONTROLREXX.1` control port. This instructs the CNet Control panel to shut down the specified port.

This is a write operation. If a user is online on the target port, a warning is included in the output but the command is still sent.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port-number>` | Yes | CNet port number to unload (0-99). Must be all digits |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | The port number that was targeted |
| `action` | string | Always `"unload"` |
| `rc` | number | ARexx return code from the control panel (0 typically indicates success) |
| `warning` | string | Always present: `"CONTROLREXX.1 commands may not take effect on CNet v5.36b"` |
| `warnings` | array | Additional warnings (present only if there are extra warnings) |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"Cannot open rexxsyslib.library"` -- library not available
- `"Port number must be 0-99"` -- invalid port number
- `"ARexx port CONTROLREXX.1 not found (CNet Control not running?)"` -- control panel not running

### Notes
- A warning is emitted if the port number exceeds the configured `HiPort` value.
- A warning is emitted if a user is currently online on the target port (the user's handle is included in the warning text, with MCI codes stripped).
- On CNet v5.36b, `CLOSEPORT` returns `RC=0` but may have no observable effect. The fixed warning in the output documents this known issue.
- Requires `rexxsyslib.library` (opened at cnet-cli startup).

---

## `cnet-cli port dump`

### Synopsis
```
cnet-cli port dump <port-number>
```

### Description
Disconnects the user on a CNet BBS port by sending the `DROPCARRIER` ARexx command to the per-port `CNETREXX{N}` ARexx port. This simulates a carrier loss, forcing the user offline.

This is a write operation. Unlike `load` and `unload`, this command targets the per-port ARexx port directly, not the control port.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port-number>` | Yes | CNet port number to dump (0-99). Must be all digits |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | The port number that was targeted |
| `action` | string | Always `"dump"` |
| `user` | string | Handle of the user who was online on the port (MCI codes stripped). Present only if a user was online at the time the command was issued |
| `rc` | number | ARexx return code from the port's ARexx handler |
| `warning` | string | `"no user online on this port"` -- present only if no user was online when the command was issued |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"Cannot open rexxsyslib.library"` -- library not available
- `"Port number must be 0-99"` -- invalid port number
- `"ARexx port CNETREXX{N} not found (port not loaded?)"` -- the target port is not loaded or not running

### Notes
- The target ARexx port name is `CNETREXX{N}` (e.g., `CNETREXX0`, `CNETREXX5`), with no zero-padding or dot separator.
- The command checks whether a user is online before sending `DROPCARRIER`. If no user is online, the `DROPCARRIER` is still sent but a `warning` field is included instead of a `user` field.
- The user check reads the in-memory `PortData` struct. There is a small race window between the check and the ARexx message delivery.
- Requires `rexxsyslib.library` (opened at cnet-cli startup).
