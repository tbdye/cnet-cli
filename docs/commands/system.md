# System Commands

Top-level commands for querying BBS status, port state, and online users. All commands in this group are read-only.

## `cnet-cli status`

### Synopsis
```
cnet-cli status
```

### Description
Returns a system overview including BBS identity, registration, port configuration, and aggregate counters. This is the simplest health-check command.

### Arguments
None.

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `system_name` | string | BBS system name (MCI codes stripped) |
| `sysop_name` | string | Sysop name (MCI codes stripped) |
| `version` | number | CNet software version number |
| `serial` | number | CNet serial number |
| `registered_to` | string | Registration holder name (MCI codes stripped) |
| `ports` | number | Number of configured ports (`nPorts`) |
| `hi_port` | number | Highest port index (`HiPort`) |
| `accounts` | number | Total account slots (`Nums[NUMS_CURRENT_ACCOUNTS]`) |
| `total_calls` | number | Lifetime call count (`Nums[NUMS_CALLS_TOTAL]`) |
| `logged_now` | number | Currently logged-in sessions (`Nums[NUMS_CALLS_LOGGED]`) |
| `subboards` | number | Total subboard count |
| `root_sub` | number | Root subboard index |
| `open_pfiles` | number | Number of open pfiles |
| `warnings` | array of string | Non-fatal warnings (e.g., missing optional libraries). Present only if warnings exist. |

### Notes
- Read-only. No semaphores are acquired; fields are read directly from the MainPort structure.
- Warnings about unavailable optional libraries (cnetmail.library, rexxsyslib.library, cnet4.library) appear here because they are accumulated during initialization.

---

## `cnet-cli ports`

### Synopsis
```
cnet-cli ports
```

### Description
Lists all ports from index 0 through HiPort, showing whether each is loaded and, if loaded, whether a user is online.

### Arguments
None.

### Options
None.

### Output Fields

Top-level object contains a `ports` array. Each element:

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | Port index (0-based) |
| `loaded` | boolean | Whether the port's PortData is allocated (not the default z0 placeholder) |
| `online` | boolean | Whether a user is logged in on this port |
| `user` | string or null | Handle of the online user (MCI stripped), or null if no user |
| `account` | number | Account number of the online user (present only when online) |
| `baud` | number | Baud rate. 0 when not online or not loaded. |
| `idle_tenths` | number | Idle time in tenths of minutes (present only when online) |

### Notes
- Read-only. Iterates `myp->PortZ[0..HiPort]` without acquiring semaphores.
- Unloaded ports have `PortZ[i]` pointing to `myp->z0` (the default PortData) or NULL.

---

## `cnet-cli who`

### Synopsis
```
cnet-cli who
```

### Description
Lists all currently online users with basic session information. Only ports with active sessions are included.

### Arguments
None.

### Options
None.

### Output Fields

Top-level object contains a `users` array. Each element:

| Field | Type | Description |
|-------|------|-------------|
| `handle` | string | User handle (MCI codes stripped) |
| `port` | number | Port index the user is on |
| `account` | number | Account number |
| `location` | string or null | Current activity description (from `MyDoing` or `Doing`), or null if unavailable |
| `idle_minutes` | number | Idle time in whole minutes (`TimeIdle / 10`) |
| `time_online_minutes` | number | Session duration in whole minutes (`TimeOnLine / 10`) |

### Notes
- Read-only. Skips unloaded and offline ports.
- `location` tries `MyDoing` (pointer to dynamic string) first, then falls back to `Doing` (fixed buffer).

---

## `cnet-cli who --detail`

### Synopsis
```
cnet-cli who --detail
```

### Description
Lists all currently online users with extended session information including access group, time-left, current subboard, carrier status, and baud rate.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--detail` | flag | -- | Enables extended output mode |

### Output Fields

Top-level object contains a `users` array. Each element:

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | Port index |
| `account` | number | Account number |
| `handle` | string | User handle (MCI codes stripped) |
| `access_group` | number | Access group number (0-31) |
| `group_name` | string or null | Access group name (MCI stripped), or null if group is out of range |
| `location` | string or null | Current activity description, or null |
| `time_online_tenths` | number | Session duration in tenths of minutes |
| `time_left_tenths` | number | Remaining time in tenths of minutes |
| `idle_tenths` | number | Idle time in tenths of minutes |
| `current_sub` | number | Physical subboard number the user is currently in |
| `carrier` | number | Carrier detect state |
| `baud` | number | Connection baud rate |
| `caller_number` | number | Sequential caller number for this session |

### Notes
- Read-only.
- Time fields are in tenths of minutes (raw precision). Divide by 10 for whole minutes.

---

## `cnet-cli who <port>`

### Synopsis
```
cnet-cli who <port>
```

### Description
Shows extended detail for a single port's online user. Returns an error if the port is not loaded or no user is online.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port>` | Yes | Port number (0-based integer) |

### Options
None.

### Output Fields

Returns a single object (not wrapped in an array) with the same fields as `who --detail`:

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | Port index |
| `account` | number | Account number |
| `handle` | string | User handle (MCI codes stripped) |
| `access_group` | number | Access group number (0-31) |
| `group_name` | string or null | Access group name (MCI stripped), or null if group is out of range |
| `location` | string or null | Current activity description, or null |
| `time_online_tenths` | number | Session duration in tenths of minutes |
| `time_left_tenths` | number | Remaining time in tenths of minutes |
| `idle_tenths` | number | Idle time in tenths of minutes |
| `current_sub` | number | Physical subboard number the user is in |
| `carrier` | number | Carrier detect state |
| `baud` | number | Connection baud rate |
| `caller_number` | number | Sequential caller number for this session |

### Notes
- Read-only.
- Returns error `"Port number out of range"` if port exceeds HiPort or >= 100.
- Returns error `"Port not loaded"` if the port's PortData is the default z0 placeholder.
- Returns error `"Port is not online"` if no user is logged in.
