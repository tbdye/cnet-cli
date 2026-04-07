# Event Commands

Event commands display CNet BBS scheduled events. Events are stored in `cnet:configs/events.cfg` as sequential 186-byte `JobType4` records. All reads are performed under `MainPort->eventsem` shared lock.

Both commands are read-only.

---

## `cnet-cli event list`

### Synopsis
```
cnet-cli event list [--all]
```

### Description
Lists all scheduled events. By default, excludes deleted events. Use `--all` to include deleted events in the output.

This is a read-only command.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--all` | flag | off | Include deleted events in the listing |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `events` | array | Array of event summary objects |
| `count` | number | Number of events in the array |

Each element of `events`:

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | Event index (0-based position in the events file) |
| `name` | string | Event name |
| `type` | string | Event type label (see Event Types below) |
| `type_id` | number | Numeric event type ID (0-22) |
| `status` | string | Event status label (see Status Values below) |
| `status_id` | number | Numeric status ID (0-3) |
| `invoke` | string | Invocation type label (see Invoke Types below) |
| `invoke_raw` | number | Raw invoke bitmask value |
| `start_time` | string/null | Start time as ISO 8601 string (`"YYYY-MM-DDTHH:MM:SS"`), or `null` if unset |
| `start_time_raw` | number | Start time as raw Amiga epoch seconds (since 1978-01-01) |
| `repeat_seconds` | number | Repeat interval in seconds (0 or negative = no repeat) |
| `deleted` | boolean | Whether the event is marked as deleted |
| `enabled` | boolean | Derived field: `true` if status is Ready (0) and not deleted |

### Notes
- If no events file exists, returns an empty array with `count: 0`.
- The `enabled` field is a convenience derived from `status_id == 0 && deleted == false`.
- Event indices are stable -- they correspond to the physical position in the events file. Deleted events leave gaps.

---

## `cnet-cli event show`

### Synopsis
```
cnet-cli event show <index>
```

### Description
Displays full detail for a single scheduled event, including schedule configuration, execution history, and per-port settings.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<index>` | Yes | Event index (0-based, must be a non-negative integer) |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `event` | object | Event detail object (see below) |

#### `event` object

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | Event index |
| `name` | string | Event name |
| `command` | string | Command/argument string (file path, ARexx script, DOS command, etc.) |
| `ports` | string | Port specification string (which ports this event targets) |
| `type` | string | Event type label (see Event Types below) |
| `type_id` | number | Numeric event type ID (0-22) |
| `status` | string | Event status label (see Status Values below) |
| `status_id` | number | Numeric status ID (0-3) |
| `invoke` | string | Invocation type label (see Invoke Types below) |
| `invoke_id` | number/null | Invocation type index (0-3), or `null` if unknown |
| `invoke_raw` | number | Raw invoke bitmask value |
| `start_time` | string/null | Start time as ISO 8601 string, or `null` if unset |
| `start_time_raw` | number | Start time as raw Amiga epoch seconds |
| `last_executed` | string/null | Last execution time as ISO 8601 string, or `null` if never executed |
| `last_executed_raw` | number | Last execution time as raw Amiga epoch seconds |
| `valid_seconds` | number | Valid duration window in seconds |
| `days` | array | Active days as array of day name strings (e.g., `["Mon", "Wed", "Fri"]`) |
| `days_raw` | number | Raw days bitmask (bit 0 = Sun, bit 1 = Mon, ..., bit 6 = Sat) |
| `deleted` | boolean | Whether the event is marked as deleted |
| `repeat_seconds` | number | Repeat interval in seconds |
| `repeat_human` | string | Human-readable repeat interval (e.g., `"1d 2h 30m"`, or `"none"`) |
| `port_action` | string | Port action label (see Port Actions below) |
| `port_action_id` | number | Numeric port action ID (0-2) |

### Notes
- The `index` argument is validated against the file size. An out-of-range index produces an error message showing the valid range.
- The `command` field contains the event's argument string, whose meaning depends on the event type: it may be a CNetC program path, ARexx script path, DOS command, file path, port number, access group string, or other type-specific value.

---

## Reference Tables

### Event Types

| ID | Label | Description |
|----|-------|-------------|
| 0 | `RunCNetC` | Run a CNetC program |
| 1 | `RunARexx` | Run an ARexx script |
| 2 | `RunDOS` | Run a DOS command |
| 3 | `ReadFile` | Read/display a file |
| 4 | `DOS-CMD` | Execute a DOS command (alternate) |
| 5 | `ClosePort` | Close a port |
| 6 | `Charges#` | Set charge rate number |
| 7 | `LogonBPS` | Set logon BPS rate |
| 8 | `DloadBPS` | Set download BPS rate |
| 9 | `ULoadBPS` | Set upload BPS rate |
| 10 | `LogonAccess` | Set logon access groups |
| 11 | `XfersAccess` | Set file transfer access groups |
| 12 | `DoorsAccess` | Set door/pfile access groups |
| 13 | `Modem#` | Set modem configuration number |
| 14 | `CallBack` | Trigger callback verification |
| 15 | `Avalid#` | Set auto-validation number |
| 16 | `Doors` | Door/pfile area control |
| 17 | `Files` | File area control |
| 18 | `MsgArea` | Message area control |
| 19 | `NewUsers` | New user registration control |
| 20 | `SysopIn` | Sysop presence control |
| 21 | `JoinLink` | Join a link |
| 22 | `On-Line` | Online status control |

### Status Values

| ID | Label | Description |
|----|-------|-------------|
| 0 | `Ready` | Event is active and will execute at its scheduled time |
| 1 | `Suspended` | Event is temporarily disabled |
| 2 | `Once/Delete` | Event will execute once then be marked deleted |
| 3 | `Cancelled` | Event has been cancelled |

### Invoke Types

| ID | Label | Description |
|----|-------|-------------|
| 0 | `Immediate-NoDump` | Execute immediately, do not dump the current user |
| 1 | `Immediate-UDump` | Execute immediately, dump (disconnect) the current user |
| 2 | `If Port Idle` | Execute only if the target port is idle |
| 3 | `If User Online` | Execute only if a user is online on the target port |

### Port Actions

| ID | Label | Description |
|----|-------|-------------|
| 0 | `Ignore` | No port action |
| 1 | `Run` | Run the event on the specified port |
| 2 | `Run/Close` | Run the event on the port and close it afterward |

### Days Bitmask

The `days_raw` field is a 7-bit bitmask:

| Bit | Day |
|-----|-----|
| 0 | Sun |
| 1 | Mon |
| 2 | Tue |
| 3 | Wed |
| 4 | Thu |
| 5 | Fri |
| 6 | Sat |

The `days` array contains the abbreviated day names for each set bit. An empty array means no specific days are configured (the event may use repeat interval instead).
