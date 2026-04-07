# Access Group Commands

Access group commands manage CNet BBS access groups (privilege templates). There are exactly 32 groups (numbered 0-31), stored in the `SysData:bbs.adata` binary file (156-byte `AccessGroup` records) and mirrored in CNet's shared-memory `MainPort->AGC[]` array. Each group defines a name, expiration policy, and a `DefPrivs` (92-byte `Privs` struct) that serves as the privilege template for all users assigned to that group.

## Concurrency

The AGC[] array is static configuration loaded at boot. No semaphores are needed for read access or single-writer mutation. The `group edit` command writes directly to the in-memory array and then persists all 32 groups to disk atomically via a temp-file + DeleteFile + Rename pattern.

---

## `cnet-cli group list`

### Synopsis
```
cnet-cli group list
```

### Description
Lists all 32 access groups with summary information. Always returns exactly 32 entries (groups 0-31), including undefined groups with empty names.

This is a read-only command.

### Arguments
None.

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `groups` | array | Array of 32 access group summary objects |
| `total` | number | Always 32 |

Each element of `groups`:

| Field | Type | Description |
|-------|------|-------------|
| `id` | number | Group number (0-31) |
| `name` | string | Group name (MCI codes stripped). Empty string if undefined. |
| `defined` | boolean | Whether the group has a non-empty name |
| `expire_days` | number | Days until account expires (0 = no expiration) |
| `expire_access` | number | Group number to move users to upon expiration |
| `daily_minutes` | number | Maximum daily online minutes allowed |
| `calls_per_day` | number | Maximum calls per day allowed |
| `idle_limit` | number | Idle timeout in minutes |
| `editor_lines` | number | Maximum lines in the message editor |
| `abits` | string | Privilege bitmask 1 as hex string (e.g., `"0x001fffff"`) |
| `abits2` | string | Privilege bitmask 2 as hex string (e.g., `"0x000000ff"`) |

### Notes
- The list always contains all 32 groups regardless of whether they are defined.
- Use the `defined` field to distinguish configured groups from empty slots.

---

## `cnet-cli group show`

### Synopsis
```
cnet-cli group show <group-number>
```

### Description
Displays full detail for a single access group, including the complete `DefPrivs` privilege template and decoded privilege flag names.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<group-number>` | Yes | Group number (0-31) |

### Options
None.

### Output Fields

Top-level object fields:

| Field | Type | Description |
|-------|------|-------------|
| `id` | number | Group number (0-31) |
| `name` | string | Group name (MCI codes stripped) |
| `defined` | boolean | Whether the group has a non-empty name |
| `expire_days` | number | Days until account expires (0 = no expiration) |
| `expire_access` | number | Group number to move users to upon expiration |
| `privileges` | object | Full DefPrivs privilege template (see below) |
| `decoded_flags` | object | Human-readable privilege flag names (see below) |

#### `privileges` object

| Field | Type | Description |
|-------|------|-------------|
| `mbase_flags` | string | Message base access bitmask as hex (e.g., `"0xffffffff"`) |
| `mbase_flags_groups` | string | Message base access as group ranges (e.g., `"0-31"`) |
| `fbase_flags` | string | File base access bitmask as hex |
| `fbase_flags_groups` | string | File base access as group ranges |
| `lbase_flags` | string | Library base access bitmask as hex |
| `lbase_flags_groups` | string | Library base access as group ranges |
| `abits` | string | Privilege bitmask 1 as hex |
| `abits2` | string | Privilege bitmask 2 as hex |
| `daily_down_bytes` | number | Maximum daily download bytes |
| `daily_up_bytes` | number | Maximum daily upload bytes |
| `calls_per_day` | number | Maximum calls per day |
| `call_minutes` | number | Maximum minutes per call |
| `daily_minutes` | number | Maximum daily online minutes |
| `daily_downloads` | number | Maximum daily download count |
| `daily_uploads` | number | Maximum daily upload count |
| `messages` | number | Maximum messages per call |
| `feedbacks` | number | Maximum feedbacks per call |
| `editor_lines` | number | Maximum lines in the message editor |
| `idle_limit` | number | Idle timeout in minutes |
| `max_mail_kbytes` | number | Maximum mail storage in KB |
| `purge_days` | number | Days of inactivity before account purge |
| `file_ratio` | number | File ratio (uploads required per download, 0 = unlimited) |
| `byte_ratio` | number | Byte ratio (upload bytes per download byte, 0 = unlimited) |
| `sig_lines` | number | Maximum signature lines |
| `daily_pfile_minutes` | number | Maximum daily minutes in doors/pfiles |
| `allow_aliases` | number | Alias permission level (0=no, 1=yes, 2=forced) |
| `delete_own` | number | Can delete own messages (0=no, 1=yes, 2=always) |
| `anonymous` | number | Anonymous posting (0=no, 1=yes, 2=forced) |
| `private_area` | number | Private file area access (0=no, 1=yes, 2=always) |
| `callback` | number | Callback verification (0=no, 1=yes, 2=forced) |
| `term_link` | number | Terminal link access (0=no, 1=yes) |
| `caller_id` | number | Caller ID verification (0=no, 1=yes) |
| `page_sysop` | number | Can page sysop (0=no, 1=yes) |
| `alias` | number | Alias configuration value |
| `dictionary` | number | Dictionary configuration value |
| `log_flags` | string | Log bitmask as hex |
| `log_to_mail` | number | Log-to-mail configuration value |

#### `decoded_flags` object

All fields are boolean. These decode the `abits` and `abits2` bitmasks into named flags.

**From `abits` (privilege bitmask 1):**

| Field | Description |
|-------|-------------|
| `email` | Can use email |
| `pfile` | Can access doors/pfiles |
| `gfile` | Can access general files |
| `ulist` | Can view user list |
| `sysop` | Has sysop privileges |
| `rewards` | Eligible for rewards |
| `autovalid` | Account is auto-validated |
| `suspended` | Account is suspended |
| `conference` | Can access conferences |
| `mci1` | MCI code set 1 enabled |
| `mci2` | MCI code set 2 enabled |
| `relogon` | Can re-logon without disconnecting |
| `receive_mail` | Can receive mail |
| `bulk_mail` | Can send bulk mail |
| `urgent_mail` | Can send urgent mail |
| `read_any` | Can read any message |
| `delete_any` | Can delete any message |
| `file_add` | Can add files |
| `see_anon` | Can see anonymous posters |
| `nolocks` | Bypasses locks |
| `vote_topic` | Can create vote topics |
| `vote_choice` | Can add vote choices |

**From `abits2` (privilege bitmask 2):**

| Field | Description |
|-------|-------------|
| `superuser` | Superuser privileges |
| `port_monitor` | Can monitor ports |
| `broadcast` | Can broadcast messages |
| `edit_handle` | Can edit own handle |
| `edit_realname` | Can edit own real name |
| `net_mail` | Can use network mail |
| `open_screen` | Can open port screen |
| `open_capture` | Can open capture file |

### Notes
- The `_groups` suffix fields (e.g., `mbase_flags_groups`) are human-readable expansions of the corresponding hex bitmask using CNet's `ExpandFlags()` function.
- The `decoded_flags` object provides the same information as `abits`/`abits2` but in a more accessible format.

---

## `cnet-cli group edit`

### Synopsis
```
cnet-cli group edit <group-number> [--name <value>] [--expire-days <N>] [--expire-access <N>] ...
```

### Description
Modifies one or more fields of an access group's configuration and privilege template. Updates the in-memory `AGC[]` array, then writes all 32 groups to `SysData:bbs.adata` via a safe temp-file + DeleteFile + Rename pattern.

At least one `--flag` must be specified. Unknown flags produce an immediate error.

This is a write command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<group-number>` | Yes | Group number to edit (0-31) |

### Options

#### Group-level fields

| Option | Type | Range | Description |
|--------|------|-------|-------------|
| `--name` | string | up to 31 chars | Group name |
| `--expire-days` | number | 0-32767 | Days until account expiration (0 = no expiration) |
| `--expire-access` | number | 0-31 | Group to move users to upon expiration |

#### Privilege short fields (16-bit signed)

| Option | Type | Range | Description |
|--------|------|-------|-------------|
| `--daily-minutes` | number | 0-32767 | Maximum daily online minutes |
| `--call-minutes` | number | 0-32767 | Maximum minutes per call |
| `--calls` | number | 0-32767 | Maximum calls per day |
| `--idle` | number | 0-32767 | Idle timeout in minutes |
| `--editor-lines` | number | 0-32767 | Maximum lines in message editor |
| `--messages` | number | 0-32767 | Maximum messages per call |
| `--feedbacks` | number | 0-32767 | Maximum feedbacks per call |
| `--daily-downloads` | number | 0-32767 | Maximum daily download count |
| `--daily-uploads` | number | 0-32767 | Maximum daily upload count |
| `--max-mail-kbytes` | number | 0-32767 | Maximum mail storage in KB |
| `--purge-days` | number | 0-32767 | Days of inactivity before purge |
| `--sig-lines` | number | 0-32767 | Maximum signature lines |
| `--daily-pfile-minutes` | number | 0-32767 | Maximum daily door/pfile minutes |
| `--log-to-mail` | number | 0-32767 | Log-to-mail configuration value |
| `--alias` | number | 0-32767 | Alias configuration value |
| `--dictionary` | number | 0-32767 | Dictionary configuration value |

#### Privilege long fields (32-bit signed)

| Option | Type | Range | Description |
|--------|------|-------|-------------|
| `--daily-down-bytes` | number | (any long) | Maximum daily download bytes |
| `--daily-up-bytes` | number | (any long) | Maximum daily upload bytes |

#### Privilege byte fields (8-bit unsigned)

| Option | Type | Range | Description |
|--------|------|-------|-------------|
| `--file-ratio` | number | 0-255 | File ratio (0 = unlimited) |
| `--byte-ratio` | number | 0-255 | Byte ratio (0 = unlimited) |
| `--allow-aliases` | number | 0-2 | Alias permission (0=no, 1=yes, 2=forced) |
| `--delete-own` | number | 0-2 | Delete own messages (0=no, 1=yes, 2=always) |
| `--anonymous` | number | 0-2 | Anonymous posting (0=no, 1=yes, 2=forced) |
| `--private-area` | number | 0-2 | Private file area (0=no, 1=yes, 2=always) |
| `--callback` | number | 0-2 | Callback verification (0=no, 1=yes, 2=forced) |
| `--term-link` | number | 0-1 | Terminal link access (0=no, 1=yes) |
| `--caller-id` | number | 0-1 | Caller ID verification (0=no, 1=yes) |
| `--page-sysop` | number | 0-1 | Can page sysop (0=no, 1=yes) |

#### Bitmask fields (hex or group-range string)

| Option | Type | Format | Description |
|--------|------|--------|-------------|
| `--mbase-flags` | hex/string | `0xNNNNNNNN` or `1-3,5` | Message base access bitmask |
| `--fbase-flags` | hex/string | `0xNNNNNNNN` or `1-3,5` | File base access bitmask |
| `--lbase-flags` | hex/string | `0xNNNNNNNN` or `1-3,5` | Library base access bitmask |
| `--abits` | hex | `NNNNNNNN` or `0xNNNNNNNN` | Privilege bitmask 1 (hex, with or without `0x` prefix) |
| `--abits2` | hex | `NNNNNNNN` or `0xNNNNNNNN` | Privilege bitmask 2 (hex, with or without `0x` prefix) |
| `--log-flags` | hex | `NNNNNNNN` or `0xNNNNNNNN` | Log bitmask (hex, with or without `0x` prefix) |

### Output Fields

On success:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"updated"` |
| `id` | number | Group number that was edited |
| `name` | string | Current group name (after edit) |
| `defined` | boolean | Whether the group has a non-empty name |
| `expire_days` | number | Current expiration days (after edit) |
| `expire_access` | number | Current expiration group (after edit) |
| `privileges` | object | Full privileges object (same structure as `group show`) |
| `decoded_flags` | object | Decoded privilege flags (same structure as `group show`) |
| `fields_changed` | array | Array of field name strings that were modified |

### Notes
- The `--mbase-flags`, `--fbase-flags`, and `--lbase-flags` options accept either hex format (`0xffffffff`) or CNet group-range format (`1-3,5,7-10`). The range format uses `ConvertAccess()` from cnet.library.
- The `--abits`, `--abits2`, and `--log-flags` options accept hex strings with or without `0x` prefix (e.g., both `001fffff` and `0x001fffff` are valid).
- All changes are applied to memory first, then persisted to disk atomically. If disk write fails but the original file is intact, the error message notes that in-memory state is updated and the command can be re-run.
- If disk write partially fails (original deleted but temp file cannot be renamed), a CRITICAL error is returned with manual recovery instructions.
- The `fields_changed` array lists the JSON field names (not the CLI flag names) of all modified fields.

---

## `cnet-cli group transpose`

### Synopsis
```
cnet-cli group transpose <group-number>
```

### Description
Pushes the access group's `DefPrivs` privilege template to all user accounts currently assigned to that group. This is a bulk operation that overwrites each matching user's `MyPrivs` with the group's `DefPrivs`.

The operation uses a two-phase mechanism for safe concurrent access:

**Phase A (Scan):** Acquires SEM[1] shared and scans the `Key[]` array to find all accounts whose `Access` field matches the target group. Collects up to 2000 candidate account numbers.

**Phase B (Apply):** For each candidate, calls `LockAccount()` to acquire an exclusive lock on the user record, re-verifies the user's group assignment (the Key[] snapshot may be stale), copies the 92-byte `DefPrivs` struct via `memcpy()`, then calls `UnLockAccount(account, 1)` to save and release.

This is a write command. It modifies user account data on disk.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<group-number>` | Yes | Group number (0-31) whose DefPrivs to push |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"transposed"` |
| `group` | number | Group number that was transposed |
| `group_name` | string | Group name (MCI codes stripped) |
| `accounts_modified` | number | Number of user accounts that were updated |
| `accounts_skipped` | number | Number of accounts that could not be locked |
| `total_scanned` | number | Total account slot count (from Nums[NUMS_CURRENT_ACCOUNTS]). Note: if this exceeds 2000, only the first 2000 are actually scanned. |
| `warnings` | array | Array of warning strings. Absent when there are no warnings (not an empty array). Present if the group has no name. |

### Notes
- The maximum number of candidate accounts is 2000. Accounts beyond this limit are silently ignored.
- Accounts whose group assignment changed between Phase A and Phase B (due to concurrent modification) are silently skipped without being counted as skipped.
- The `accounts_skipped` count reflects only accounts where `LockAccount()` failed (e.g., already locked by another process).
- A warning is emitted if the group has an empty name.
- This command uses `LockAccount()`/`UnLockAccount()` from cnet.library for safe per-account locking. Each account is persisted individually via `UnLockAccount(account, 1)`.
