# User Commands

User management commands for CNet BBS administration. All user commands are
accessed via `cnet-cli user <subcommand>`. The `who` and `olm` commands are
top-level commands (not under the `user` prefix) but are documented here
because they operate on users/ports.

## User Resolution

All commands that accept `<account|handle>` resolve the identifier using
`resolve_user_full()`:

1. If the string is all digits, it is treated as a 1-based account number.
   Valid range: 1 to `Nums[NUMS_CURRENT_ACCOUNTS]`.
2. Otherwise, it is treated as a handle and looked up via `FindHandle()`
   from cnet.library using the `IName[]` sorted index under `SEM[1]`
   shared lock.

Account numbers are 1-based. Resolution returns -1 if the user is not found.

## `cnet-cli user list`

### Synopsis
```
cnet-cli user list [--group N] [--limit N] [--offset N]
```

### Description
Lists user accounts from the in-memory `Key[]` array. Returns summary
information for each user. Empty account slots (handle is empty string)
are skipped. Read-only operation.

### Arguments
None (all parameters are options).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--group` | integer (0-31) | none (no filter) | Filter by access group number |
| `--limit` | integer | none (no limit) | Maximum number of users to return |
| `--offset` | integer | 0 | Number of matching users to skip before emitting |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `users` | array | Array of user summary objects |
| `total_slots` | number | Total account slots allocated (`Nums[NUMS_CURRENT_ACCOUNTS]`) |
| `matched` | number | Number of users emitted in the `users` array |

Each element in `users`:

| Field | Type | Description |
|-------|------|-------------|
| `account` | number | 1-based account number |
| `handle` | string | User handle (MCI codes stripped) |
| `real_name` | string or null | Real name (null if PName privacy flag is set) |
| `access_group` | number | Access group number (0-31) |
| `group_name` | string or null | Access group name (null if group number is out of range) |
| `uucp` | string | UUCP name (mail username) |
| `id_number` | number | Unique persistent ID number |

### Notes
- Acquires `SEM[1]` shared lock for the duration of the scan.
- Offset/limit apply after group filtering.
- Requires cnet.library (for `MCIRemove` MCI stripping).

---

## `cnet-cli user show`

### Synopsis
```
cnet-cli user show <account|handle>
```

### Description
Shows full detail for a single user account. Locks the account via
`LockAccount()`, reads all `UserData` fields, then unlocks with save=0
(no changes). Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle to look up |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `account` | number | 1-based account number |
| `id_number` | number | Unique persistent ID number |
| `handle` | string | User handle (MCI stripped) |
| `real_name` | string | Real name (MCI stripped; always shown regardless of PName flag) |
| `uucp` | string | UUCP name (mail username) |
| `address_type` | string | `"local"`, `"internet"`, or `"unknown"` (from the local `address_type()` helper in util.c) |
| `address` | string | Street address |
| `city_state` | string | City and state |
| `zip_code` | string | ZIP/postal code |
| `country` | string | Country |
| `phone_data` | string | Data phone number |
| `phone_voice` | string | Voice phone number |
| `organization` | string | Organization/company |
| `comments` | string | Sysop comments |
| `banner` | string | User banner (MCI stripped) |
| `access_group` | number | Access group number (0-31) |
| `group_name` | string or null | Access group name |
| `expire_access` | number | Access group to revert to on expiration |
| `suspended` | boolean | Whether the account is suspended (SUSPENDACCT_FLAG in ABits) |
| `phone_verified` | number | Phone verification status |
| `birthdate` | string or null | ISO 8601 date (null if unset) |
| `first_call` | string or null | First call date (null if unset) |
| `last_call` | string or null | Last call date (null if unset) |
| `expire_date` | string or null | Account expiration date (null if unset) |
| `total_calls` | number | Total number of calls |
| `pub_messages` | number | Public messages posted |
| `pri_messages` | number | Private messages sent |
| `up_kbytes` | number | Kilobytes uploaded |
| `up_files` | number | Files uploaded |
| `down_kbytes` | number | Kilobytes downloaded |
| `down_files` | number | Files downloaded |
| `file_credits` | number | File download credits |
| `byte_credits` | number | Byte download credits |
| `time_credits` | number | Time credits (minutes) |
| `balance` | number | Account balance |
| `door_points` | number | Door game points |
| `term_width` | number | Terminal width (columns) |
| `term_length` | number | Terminal length (rows) |
| `colors` | number | Color mode |
| `ansi` | number | ANSI mode |
| `abits` | string | Privilege bits A as hex string (e.g. `"0x00000000"`) |
| `abits2` | string | Privilege bits B as hex string |
| `daily_minutes` | number | Daily time limit (minutes) |
| `idle_limit` | number | Idle timeout (minutes) |
| `editor_lines` | number | Editor line limit |

### Notes
- Password is never emitted (security).
- Dates are formatted as `YYYY-MM-DDTHH:MM:SS` (ISO 8601 without timezone).
- Uses `LockAccount()`/`UnLockAccount()` from cnet.library.
- Unlike `user profile`, this shows the real name regardless of the PName privacy flag.

---

## `cnet-cli user find`

### Synopsis
```
cnet-cli user find <query> [--phone]
```

### Description
Searches for users matching a query string. By default, performs a
case-insensitive substring match on both Handle and RealName. With
`--phone`, performs an exact phone number lookup via `FindPhone()`.
Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<query>` | Yes | Search string (handle/name substring, or phone number with `--phone`) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--phone` | flag | off | Search by phone number instead of handle/name |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `users` | array | Array of matching user summary objects (same schema as `user list`) |
| `matched` | number | Number of matching users found |

Each element in `users` has the same fields as `user list` (account, handle,
real_name, access_group, group_name, uucp, id_number).

### Notes
- Without `--phone`: iterates all accounts under `SEM[1]` shared lock,
  uses `ci_contains()` for case-insensitive substring matching.
- With `--phone`: calls `FindPhone()` from cnet.library which does an
  exact match against the `IPhone[]` sorted index. Returns at most one result.

---

## `cnet-cli user plan`

### Synopsis
```
cnet-cli user plan <account|handle>
```

### Description
Reads the user's plan file (`mail:users/{uucp}/_plan`). Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `account` | number | 1-based account number |
| `handle` | string | User handle (MCI stripped) |
| `uucp` | string | UUCP name |
| `plan` | string or null | Plan file contents (null if no plan file exists) |

### Notes
- Maximum plan file size read is 4096 bytes. Content beyond this is truncated.
- Requires the user to have a UUCP name (errors if empty).

---

## `cnet-cli user edit`

### Synopsis
```
cnet-cli user edit <account|handle> [--handle H] [--realname N] [--comment C]
    [--address A] [--city C] [--country C] [--phone-data P] [--phone-voice P]
    [--organization O] [--banner B] [--access N]
```

### Description
Edits one or more fields on a user account. Locks the account, applies
changes, then unlocks with save=1. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle to edit |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--handle` | string | no change | New handle |
| `--realname` | string | no change | New real name |
| `--comment` | string | no change | New sysop comments |
| `--address` | string | no change | New street address |
| `--city` | string | no change | New city/state |
| `--country` | string | no change | New country |
| `--phone-data` | string | no change | New data phone number |
| `--phone-voice` | string | no change | New voice phone number |
| `--organization` | string | no change | New organization |
| `--banner` | string | no change | New banner text |
| `--access` | integer (0-31) | no change | New access group number |

At least one option must be specified. Unknown `--` flags cause an error.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"updated"` |
| `account` | number | Account number that was edited |
| `fields_changed` | array of strings | List of field names that were changed |
| `note` | string | Present only if handle was changed; explains CNet restart is needed |
| `warnings` | array of strings | Present if warnings occurred (e.g., user is online) |

### Notes
- Uses `LockAccount()`/`UnLockAccount()` with save=1.
- If the user is currently online, a warning is emitted: changes may be
  overwritten when the user logs off (CNet writes the in-memory copy back).
- Handle changes require a CNet restart to update the user list cache.
- String fields are copied with `safe_strcpy()` which null-terminates within
  the struct field size.

---

## `cnet-cli user disable`

### Synopsis
```
cnet-cli user disable <account|handle>
```

### Description
Suspends a user account by setting the SUSPENDACCT_FLAG bit in the user's
privilege bits (ABits). Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account to suspend |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"disabled"` |
| `account` | number | Account number |
| `handle` | string | User handle (MCI stripped) |
| `was_already_disabled` | boolean | True if the account was already suspended before this call |

### Notes
- Uses `LockAccount()`/`UnLockAccount()` with save=1.
- Idempotent: calling on an already-suspended account succeeds and reports
  `was_already_disabled: true`.

---

## `cnet-cli user enable`

### Synopsis
```
cnet-cli user enable <account|handle>
```

### Description
Unsuspends a user account by clearing the SUSPENDACCT_FLAG bit. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account to unsuspend |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"enabled"` |
| `account` | number | Account number |
| `handle` | string | User handle (MCI stripped) |
| `was_already_enabled` | boolean | True if the account was not suspended before this call |

### Notes
- Uses `LockAccount()`/`UnLockAccount()` with save=1.
- Idempotent: calling on an already-enabled account succeeds and reports
  `was_already_enabled: true`.

---

## `cnet-cli user profile`

### Synopsis
```
cnet-cli user profile <account|handle>
```

### Description
Returns a public-facing subset of user information. Respects the PName
privacy flag from `UserData` (not from `Key[]`). Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `account` | number | 1-based account number |
| `handle` | string | User handle (MCI stripped) |
| `real_name` | string or null | Real name (null if PName privacy flag is set) |
| `access_group` | number | Access group number |
| `group_name` | string or null | Access group name |
| `organization` | string | Organization |
| `banner` | string | User banner (MCI stripped) |
| `last_call` | string or null | Last call date (ISO 8601, null if unset) |
| `first_call` | string or null | First call date (ISO 8601, null if unset) |
| `total_calls` | number | Total calls |
| `pub_messages` | number | Public messages posted |
| `up_files` | number | Files uploaded |
| `up_kbytes` | number | Kilobytes uploaded |
| `down_files` | number | Files downloaded |
| `down_kbytes` | number | Kilobytes downloaded |
| `suspended` | boolean | Whether account is suspended |

### Notes
- Unlike `user show`, this command respects the PName privacy flag for the
  real name field and omits private information (address, phone, etc.).
- Uses `LockAccount()`/`UnLockAccount()` with save=0.

---

## `cnet-cli user delete`

### Synopsis
```
cnet-cli user delete <account|handle> --force
```

### Description
Permanently deletes a user account. Zeros the `UserData` fields, clears
the `Key[]` entry, rebuilds the `IName[]` and `IPhone[]` indexes, decrements
the in-use account count, and writes updated index files to disk. This is a
destructive, irreversible write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account to delete |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--force` | flag | required | Confirms the destructive operation. Command fails without this flag. |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"deleted"` |
| `account` | number | Account number that was deleted |
| `handle` | string | Handle of the deleted user |
| `real_name` | string | Real name of the deleted user |
| `uucp` | string | UUCP name of the deleted user |
| `files_written` | array of strings | Index files successfully written (subset of `"bbs.ukeys4"`, `"bbs.uind1"`, `"bbs.uind2"`, `"bbs.sdata"`) |
| `warnings` | array of strings | Warnings (e.g., index rebuild failures, file write failures). Always present (may be an empty array). |

### Notes
- Account #1 (sysop) cannot be deleted.
- Users currently online cannot be deleted.
- Already-empty account slots are rejected.
- Acquires `SEM[1]` exclusive lock for Key[]/IName[]/IPhone[] updates.
- Acquires `SEM[4]` exclusive lock for Nums[] decrement.
- Writes four index files to `sysdata:`: `bbs.ukeys4`, `bbs.uind1`,
  `bbs.uind2`, `bbs.sdata`.
- The user's mail directory under `mail:users/` is NOT deleted.

---

## `cnet-cli who`

### Synopsis
```
cnet-cli who
cnet-cli who --detail
cnet-cli who <port>
```

### Description
Lists online users. The base command returns a compact summary. With
`--detail`, returns extended information for all online users. With a
port number, returns extended information for a single port. Read-only.

Note: `who` is a top-level command, not under the `user` prefix.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `--detail` | No | Show extended detail for all online users |
| `<port>` | No | Show extended detail for a single port number |

### Options
None (arguments control mode).

### Output Fields (basic mode, no arguments)

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `users` | array | Array of online user objects |

Each element in `users`:

| Field | Type | Description |
|-------|------|-------------|
| `handle` | string | User handle (MCI stripped) |
| `port` | number | Port number |
| `account` | number | Account number |
| `location` | string or null | Current activity description (MyDoing or Doing) |
| `idle_minutes` | number | Idle time in whole minutes (TimeIdle / 10) |
| `time_online_minutes` | number | Time online in whole minutes (TimeOnLine / 10) |

### Output Fields (--detail mode or single port)

For `--detail`, the top-level object has a `users` array. For a single port
number, the top-level IS the port detail object (no wrapper).

Each port detail object:

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | Port number |
| `account` | number | Account number |
| `handle` | string | User handle (MCI stripped) |
| `access_group` | number | Access group number |
| `group_name` | string or null | Access group name |
| `location` | string or null | Current activity description |
| `time_online_tenths` | number | Time online in tenths of minutes |
| `time_left_tenths` | number | Time remaining in tenths of minutes |
| `idle_tenths` | number | Idle time in tenths of minutes |
| `current_sub` | number | Current subboard number |
| `carrier` | number | Carrier detect status |
| `baud` | number | Baud rate |
| `caller_number` | number | Sequential caller number for this session |

### Notes
- Iterates `PortZ[]` from port 0 to `HiPort` (max 100).
- Unloaded ports (pointing to `z0`) are skipped in basic and `--detail` modes.
- Single port mode returns an error if the port is not loaded or not online.

---

## `cnet-cli olm`

### Synopsis
```
cnet-cli olm <port> --from <account|handle> --text "message" [--broadcast]
```

### Description
Sends an On-Line Message (OLM) to a user on a specific port. First attempts
`FileOLM()` from cnet.library. If that fails (returns 0), falls back to
direct file I/O writing a 64-byte header + message text to
`{OLMpath}_aolm{port}`. Write operation.

Note: `olm` is a top-level command, not under the `user` prefix.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port>` | Yes | Target port number (must be numeric, 0 to HiPort) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--from` | string | required | Sender account number or handle |
| `--text` | string | required | Message text (max 380 characters) |
| `--broadcast` | flag | off | Set OLM_BROADCAST flag (0x00000001) |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"sent"` |
| `method` | string | `"fileolm"` if FileOLM succeeded, `"direct_io"` if fallback was used |
| `port` | number | Target port number |
| `to_handle` | string | Handle of the recipient (MCI stripped) |
| `to_account` | number | Account number of the recipient |
| `from_handle` | string | Handle of the sender (MCI stripped) |
| `from_account` | number | Account number of the sender |
| `broadcast` | boolean | Present and true only if `--broadcast` was used |

### Notes
- The target port must be loaded and have a user online.
- Text is limited to 380 characters.
- Direct I/O fallback writes a 64-byte header with: ByID (4 bytes),
  ByUser (26 bytes), ByAccount (2 bytes), Port (2 bytes), broadcast flag
  (1 byte), padding (1 byte), date (4 bytes), and 24 bytes of zeros.
  The timestamp uses `CNetTime()` from cnet4.library if available.
- If the OLM file already exists (CNet is displaying a pending OLM),
  the direct I/O fallback will fail with "Target has pending OLM".
- Unknown options are rejected with an error.
- Requires cnet.library. cnet4.library is optional (used for `CNetTime()`
  timestamp in direct I/O fallback).
