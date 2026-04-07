# Mail Commands

Mail management commands for CNet BBS administration. All mail commands are
accessed via `cnet-cli mail <subcommand>`. Mail alias management commands are
nested under `cnet-cli mail alias <subcommand>`.

## Implementation Notes

All mail operations use **direct file I/O** against CNet's on-disk mail
format rather than the CNet `SendMail` API. The on-disk format uses 810-byte
header records (`_mhead4`) and variable-length body text (`_mtext4`).

**Important:** The on-disk mail header format (810 bytes) does NOT match the
SDK's `MailHeader4` struct (528 bytes). cnet-cli uses its own `MailHeaderDisk`
struct verified empirically against the live BBS.

Per-account mail semaphores from `GetMailSems()` (cnetmail.library) protect
concurrent file access. Read operations use shared locks; write operations
use exclusive locks.

## User Resolution

All commands that accept `<account|handle>` resolve the identifier using
`resolve_user_full()` (see user.md for details). For mail commands, the UUCP
name must also be non-empty; the command fails if the resolved account has no
UUCP name.

## Library Dependencies

| Library | Required | Used For |
|---------|----------|----------|
| cnet.library | Yes | User resolution, `MCIRemove`, `CreateMailDir`, `CreateFolderName`, `BuildDir`, `FileSize`, `CNetReadDir`, `CNetAddressToAccount` |
| cnetmail.library | Yes (for most commands) | `GetMailSems()` for per-account mail semaphores |
| cnet4.library | No | Not used by mail commands |

---

## `cnet-cli mail send`

### Synopsis
```
cnet-cli mail send --from <user> --to <user> --subject <text> --body <text> [--sentmail]
```

### Description
Sends a mail message from one user to another. Writes directly to the
recipient's INBOX folder via file I/O. Optionally saves a copy to the
sender's SENTMAIL folder. Write operation.

### Arguments
None (all parameters are options).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--from` | string | required | Sender account number or handle |
| `--to` | string | required | Recipient account number or handle |
| `--subject` | string | required | Message subject (max 79 characters) |
| `--body` | string | required | Message body text (max 65535 bytes) |
| `--sentmail` | flag | off | Save a copy to the sender's SENTMAIL folder |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"sent"` |
| `from` | string | Sender's UUCP name |
| `to` | string | Recipient's UUCP name |
| `subject` | string | The subject as sent |
| `sentmail_copy` | boolean | Present and true only if `--sentmail` was used and the copy succeeded |
| `warning` | string | Present only if SENTMAIL copy failed (primary delivery still succeeded) |

### Notes
- Requires cnetmail.library for mail semaphores.
- Subject is truncated to 79 characters at validation; body is limited to
  65535 bytes (USHORT max for body length field).
- The sender's handle is looked up from `Key[]` under `SEM[1]` shared lock
  and stored in the mail header's `from_name` field (max 26 characters).
- Mail is delivered to `INBOX` via `CreateFolderName()` + direct file append.
- SENTMAIL copy failure is non-fatal; the success response includes a
  `warning` field if it fails.

---

## `cnet-cli mail list`

### Synopsis
```
cnet-cli mail list <account|handle> [--folder <name>] [--limit N] [--offset N]
```

### Description
Lists mail headers in a user's mail folder. Returns header summaries
without body text. Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle of the mailbox owner |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--folder` | string | `"INBOX"` | Mail folder name |
| `--limit` | integer | 50 | Maximum number of headers to return |
| `--offset` | integer | 0 | Number of records to skip from the beginning |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `account` | string | UUCP name of the mailbox owner |
| `account_number` | number | 1-based account number |
| `folder` | string | Folder name queried |
| `total` | number | Total number of messages in the folder |
| `unread` | number | Number of unread messages in the folder |
| `offset` | number | Offset applied |
| `limit` | number | Limit applied |
| `messages` | array | Array of mail header objects |

Each element in `messages`:

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | 0-based message index within the folder |
| `subject` | string | Message subject (max 80 chars) |
| `from` | string | Sender name (max 27 chars) |
| `from_account` | number | Sender's account number |
| `send_date` | number | Send timestamp as Unix epoch seconds |
| `unread` | boolean | True if the message has not been read |
| `body_length` | number | Length of the body text in bytes |
| `original_folder` | string or null | Original folder name from the header (null if empty) |

### Notes
- Requires cnetmail.library.
- Offset and limit are clamped to the total record count.
- All headers are read into memory to compute the unread count; only the
  requested range is emitted.

---

## `cnet-cli mail read`

### Synopsis
```
cnet-cli mail read <account|handle> <num> [--folder <name>]
```

### Description
Reads a single mail message including the full body text. Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle of the mailbox owner |
| `<num>` | Yes | 0-based message index within the folder |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--folder` | string | `"INBOX"` | Mail folder name |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | 0-based message index |
| `subject` | string | Message subject |
| `from` | string | Sender name |
| `from_account` | number | Sender's account number |
| `send_date` | number | Send timestamp as Unix epoch seconds |
| `unread` | boolean | True if the message has not been read |
| `folder` | string | Folder name queried |
| `original_folder` | string or null | Original folder name from the header |
| `body` | string or null | Full message body text (null if body could not be read) |

### Notes
- Requires cnetmail.library.
- The message index is 0-based. Out-of-range indices produce an error with
  the valid range.
- Does NOT mark the message as read (no write-back of read_date).
- Body text is read from `_mtext4` at the seek offset stored in the header.

---

## `cnet-cli mail reply`

### Synopsis
```
cnet-cli mail reply <account|handle> <num> --body <text>
    [--folder <name>] [--from <user>] [--sentmail]
```

### Description
Replies to a mail message. Reads the original message header to determine
the sender, constructs a "RE: " subject line, and delivers the reply to the
original sender's INBOX. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle of the mailbox containing the message to reply to |
| `<num>` | Yes | 0-based message index of the message to reply to |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--body` | string | required | Reply body text (max 65535 bytes) |
| `--folder` | string | `"INBOX"` | Folder containing the original message |
| `--from` | string | same as `<account\|handle>` | Override the reply sender (account number or handle) |
| `--sentmail` | flag | off | Save a copy to the sender's SENTMAIL folder |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"sent"` |
| `in_reply_to` | number | Index of the original message |
| `from` | string | Sender's UUCP name (the reply author) |
| `to` | string | Recipient's UUCP name (original message sender) |
| `subject` | string | Reply subject (prefixed with "RE: " if not already) |
| `sentmail_copy` | boolean | Present and true only if `--sentmail` was used and the copy succeeded |
| `warning` | string | Present only if SENTMAIL copy failed |

### Notes
- Requires cnetmail.library.
- The reply subject is automatically prefixed with "RE: " unless the original
  subject already starts with "RE: " (case-insensitive check). Subject is
  truncated to 79 characters.
- The `original_date` field in the reply header is set to the original
  message's `send_date`, preserving the conversation thread timestamp.
- If the original sender's account no longer exists or has no UUCP name,
  the reply fails with a descriptive error.
- Uses `CNetAddressToAccount()` to verify the reply target exists.
- Without `--from`, the reply is sent from the mailbox owner's account.

---

## `cnet-cli mail delete`

### Synopsis
```
cnet-cli mail delete <account|handle> <num> [--folder <name>]
```

### Description
Deletes a mail message. If deleting from a folder other than TRASHCAN, the
message is moved to TRASHCAN (both header and body text are copied). If
deleting from TRASHCAN, the message is permanently removed. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle of the mailbox owner |
| `<num>` | Yes | 0-based message index to delete |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--folder` | string | `"INBOX"` | Source folder to delete from |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"moved_to_trashcan"` or `"permanently_deleted"` (if source was TRASHCAN) |
| `deleted_index` | number | Index of the deleted message |
| `folder` | string | Source folder name |
| `account` | string | UUCP name of the mailbox owner |
| `warnings` | array of strings | Present if warnings occurred (e.g., partial write to source) |

### Notes
- Requires cnetmail.library.
- Acquires an exclusive mail semaphore (write operation).
- When deleting from a non-TRASHCAN folder:
  1. The message body is appended to TRASHCAN's `_mtext4`.
  2. The header's seek offset is updated to point to the TRASHCAN copy.
  3. The header is appended to TRASHCAN's `_mhead4`.
  4. The original header is removed from the source by rewriting the file
     without the deleted record.
- When deleting from TRASHCAN: the header is simply removed from `_mhead4`.
  If it was the last record, both `_mhead4` and `_mtext4` are truncated to
  empty files.
- TRASHCAN directory is created automatically if it does not exist
  (`CreateMailDir` + `BuildDir`).
- If the body text cannot be read for a non-TRASHCAN delete, the operation
  aborts with an error (data preservation).

---

## `cnet-cli mail folders`

### Synopsis
```
cnet-cli mail folders <account|handle>
```

### Description
Lists all mail folders for a user account with message counts. Read-only
operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `account` | string | UUCP name |
| `account_number` | number | 1-based account number |
| `folders` | array | Array of folder info objects |

Each element in `folders`:

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Folder name (directory name under FOLDERS/) |
| `total` | number | Total number of messages |
| `unread` | number | Number of unread messages |

### Notes
- Requires cnetmail.library.
- Scans `mail:users/{uucp}/FOLDERS` using `CNetReadDir()`.
- Only directory entries are enumerated (type `CNFE_TYPE_DIR`).
- Maximum 64 folders are enumerated.
- Each folder's `_mhead4` is scanned record-by-record to count total and
  unread messages.
- Returns an error if no folders are found.

---

## `cnet-cli mail count`

### Synopsis
```
cnet-cli mail count <account|handle> [--folder <name>]
```

### Description
Returns message counts (total, unread, read) for a specific mail folder.
Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--folder` | string | `"INBOX"` | Folder name to count |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `account` | string | UUCP name |
| `account_number` | number | 1-based account number |
| `folder` | string | Folder name queried |
| `total` | number | Total messages in the folder |
| `unread` | number | Unread messages |
| `read` | number | Read messages (total - unread) |

### Notes
- Requires cnetmail.library.
- Reads all headers into memory to compute counts.

---

## `cnet-cli mail feedback`

### Synopsis
```
cnet-cli mail feedback [<num>] [--folder <name>] [--limit N] [--offset N]
```

### Description
Convenience command for reading sysop feedback mail (account #1). Dispatches
to `mail list` (no number) or `mail read` (with number) with account "1"
injected. Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<num>` | No | 0-based message index to read (if omitted, lists messages) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--folder` | string | `"INBOX"` | Folder name |
| `--limit` | integer | 50 | Maximum messages to list (list mode only) |
| `--offset` | integer | 0 | Messages to skip (list mode only) |

### Output Fields

Same as `mail list` (when no number is given) or `mail read` (when a number
is given). See those commands for field documentation.

### Notes
- Always operates on account #1 (the sysop account).
- Detection: if the first non-flag argument is all digits, it is interpreted
  as a mail number (read mode). Otherwise, the command operates in list mode.
- Maximum 12 total argv elements are forwarded to the underlying command.

---

## `cnet-cli mail verify`

### Synopsis
```
cnet-cli mail verify <account|handle> [--limit N] [--offset N]
```

### Description
Convenience command to view a user's sent mail. Delegates to `mail list`
with `--folder SentMail` injected. Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--limit` | integer | 50 | Maximum messages to return |
| `--offset` | integer | 0 | Messages to skip |

### Output Fields

Same as `mail list`. The `folder` field will be `"SentMail"`.

### Notes
- The `--folder SentMail` is injected before any user-provided flags, so
  a user-supplied `--folder` would override it (last `--folder` wins).
- Maximum 12 total argv elements are forwarded.

---

# Mail Alias Commands

Mail alias management for CNet BBS. Aliases are stored in
`mail:users/{uucp}/aliases` as a flat array of 134-byte `MailAlias` records
with no file header.

The alias record format was reconstructed from binary analysis (not from the
SDK, which only forward-declares `struct MailAlias`):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 24 | Alias | Shortcut name (max 20 usable characters) |
| 24 | 24 | Name | Recipient handle or name (max 23 characters) |
| 48 | 80 | Address | Network address (blank for local aliases) |
| 128 | 2 | expansion | Reserved, zero-filled |
| 130 | 4 | next_ptr | Stale runtime list pointer, zero-filled |

### Alias Types

The `type` field in alias list output is classified as:

| Type | Condition |
|------|-----------|
| `"forward"` | Alias name is `"+"` (single plus sign) |
| `"network"` | Address field is non-empty |
| `"local"` | Address field is empty (local BBS user) |

---

## `cnet-cli mail alias list`

### Synopsis
```
cnet-cli mail alias list <account|handle>
```

### Description
Lists all mail aliases for a user account. Read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `aliases` | array | Array of alias objects |
| `count` | number | Total number of aliases |
| `account` | number | 1-based account number |
| `uucp_name` | string | UUCP name of the account |

Each element in `aliases`:

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | 0-based index of this alias in the file |
| `alias` | string | Alias shortcut name |
| `name` | string | Recipient handle or name |
| `address` | string | Network address (empty string for local aliases) |
| `type` | string | `"forward"`, `"network"`, or `"local"` |

### Notes
- If cnetmail.library is available, acquires a shared semaphore lock for
  the read. If cnetmail.library is not available, the read proceeds without
  locking (safe for single-threaded use).
- If no alias file exists, returns an empty `aliases` array with `count: 0`.

---

## `cnet-cli mail alias add`

### Synopsis
```
cnet-cli mail alias add <account|handle> --alias <name> --name <recipient> [--address <addr>]
```

### Description
Adds a new mail alias for a user account. Appends a 134-byte record to the
alias file. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--alias` | string | required | Alias shortcut name (max 20 characters) |
| `--name` | string | required | Recipient handle or name (max 23 characters) |
| `--address` | string | `""` (empty) | Network address (max 79 characters; omit for local aliases) |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"added"` |
| `alias` | string | The alias name that was added |
| `name` | string | The recipient name |
| `address` | string | The network address (empty string if not provided) |
| `account` | number | 1-based account number |
| `uucp_name` | string | UUCP name of the account |
| `total_aliases` | number | Total number of aliases after the addition |

### Notes
- Requires cnetmail.library (needed for exclusive mail semaphore).
- Validates field lengths before writing:
  - Alias: max 20 characters
  - Name: max 23 characters
  - Address: max 79 characters
- Does NOT check for duplicate aliases. Multiple aliases with the same name
  can be added.
- The alias file is created automatically if it does not exist
  (`MODE_READWRITE`).

---

## `cnet-cli mail alias remove`

### Synopsis
```
cnet-cli mail alias remove <account|handle> --alias <name> [--name <recipient>]
```

### Description
Removes mail aliases matching the given alias name. Rewrites the alias file
without the matching records. Write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes | Account number or handle |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--alias` | string | required | Alias name to remove (case-insensitive match) |
| `--name` | string | no filter | If specified, only remove aliases matching both `--alias` and this recipient name (case-insensitive) |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"removed"` |
| `alias` | string | The alias name that was matched |
| `removed_count` | number | Number of alias records removed |
| `account` | number | 1-based account number |
| `uucp_name` | string | UUCP name of the account |
| `total_aliases` | number | Total number of aliases remaining after removal |

### Notes
- Requires cnetmail.library (needed for exclusive mail semaphore).
- Matching is case-insensitive on both `--alias` and `--name` (uses
  `strcasecmp`).
- Without `--name`, ALL aliases with the matching alias name are removed.
  With `--name`, only aliases matching both the alias name and recipient
  name are removed.
- If no matching aliases are found, the command fails with an error.
- Reads all records into memory, filters out matches, and rewrites the
  entire file. Uses `AllocMem`/`FreeMem` (AmigaOS allocation) for the
  record buffer.
