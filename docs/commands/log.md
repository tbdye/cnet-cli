# Log Commands

Commands for reading CNet BBS log files. All log commands are read-only.

## `cnet-cli log list`

### Synopsis
```
cnet-cli log list
```

### Description
Enumerates all files in the `sysdata:log/` directory with their sizes and modification dates. Subdirectories are excluded.

### Arguments
None.

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `directory` | string | Always `"sysdata:log"` |
| `files` | array | List of log file entries |
| `total` | number | Total number of files found |

Each element of `files`:

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Log file name |
| `size` | number | File size in bytes |
| `date` | string | File modification date as ISO 8601 (`YYYY-MM-DDTHH:MM:SS`) |

### Notes
- Read-only.
- The date is converted from AmigaOS DateStamp format (days since 1978-01-01).

---

## `cnet-cli log read`

### Synopsis
```
cnet-cli log read <logname> [--tail N] [--lines N]
```

### Description
Reads a named log file from `sysdata:log/` and returns its lines as a JSON array. Supports tail (last N lines) and head (first N lines) modes. When neither is specified, returns the last 1000 lines by default.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<logname>` | Yes | Name of the log file (no path separators allowed) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--tail` | number | -- | Return only the last N lines |
| `--lines` | number | -- | Return only the first N lines |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `log` | string | The log file name that was read |
| `lines` | array of string | The log file lines |
| `count` | number | Number of lines returned |
| `truncated` | boolean | True if output was capped (file exceeded 512 KB read limit, or default 1000-line cap applied) |

### Notes
- Read-only. File is read under SEM[12] shared lock.
- `--tail` and `--lines` are mutually exclusive. Specifying both produces an error.
- The log file name is validated: path separators (`/`, `:`, `\`), `.`, and `..` are rejected.
- Maximum read size is 512 KB. If the file exceeds this, only the last 512 KB is read and the first partial line is discarded.
- When neither `--tail` nor `--lines` is specified, a default cap of 1000 lines is applied (showing the last 1000 lines). If this cap truncates the output, `truncated` is set to true.
- Maximum line pointer tracking is 10000 lines.

---

## `cnet-cli log callers`

### Synopsis
```
cnet-cli log callers [--tail N]
```

### Description
Shortcut for reading the `calls` log file. Equivalent to `log read calls [--tail N]` but only supports `--tail` (not `--lines`). Returns raw log lines.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--tail` | number | -- | Return only the last N lines |

### Output Fields

Same as `log read`:

| Field | Type | Description |
|-------|------|-------------|
| `log` | string | Always `"calls"` |
| `lines` | array of string | Raw callers log lines |
| `count` | number | Number of lines returned |
| `truncated` | boolean | True if output was capped |

### Notes
- Read-only. Same read limits and semaphore behavior as `log read`.

---

## `cnet-cli log callers-parsed`

### Synopsis
```
cnet-cli log callers-parsed [--tail N]
```

### Description
Reads the `calls` log and parses it into structured JSON records. Each record represents one call session, with parsed header, events, user details, SIGNON/SIGNOFF data, and SAM counters.

The callers log uses a column-based format with `.` lines as record delimiters. MCI escape sequences (0x19 + command byte) are stripped before parsing.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--tail` | number | -- | Return only the last N parsed records |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `records` | array | Parsed call session records |
| `count` | number | Number of records emitted |
| `truncated` | boolean | True if the file was truncated during reading, or the last record was unterminated |

Each element of `records`:

| Field | Type | Description |
|-------|------|-------------|
| `date` | string | Date from the header line (format: `"DD-Mon"`, e.g., `"07-Apr"`) |
| `time` | string | Time from the header line (format: `"HH:MM"`) |
| `port` | number | Port number, or -1 if not parsed |
| `connect` | string | Connect string from the header line (e.g., baud/protocol info) |
| `events` | array | Chronological list of session events |
| `user` | object or null | User detail extracted from the post-SIGNON detail line |
| `signon` | object or null | SIGNON event data, or null if no SIGNON in this record |
| `signoff_reason` | string or null | SIGNOFF reason text, or null if no SIGNOFF |
| `sam` | object or null | SAM summary counters from the post-SIGNOFF line, or null if none |

Each element of `events`:

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Event time (`"HH:MM"`) |
| `event` | string | Event label (e.g., `"SIGNON"`, `"SIGNOFF"`, `"MsgBase"`, `"FileTxfer"`) |
| `detail` | string | Event detail text |

`user` object (when present):

| Field | Type | Description |
|-------|------|-------------|
| `handle` | string | User handle |
| `real_name` | string | User's real name |
| `phone` | string | Phone number |
| `verification` | string | Verification status (extracted from parentheses) |
| `country` | string | Country (text after the closing parenthesis) |

`signon` object (when present):

| Field | Type | Description |
|-------|------|-------------|
| `account` | number | Account number |
| `caller` | number | Sequential caller number |

`sam` object (when present):

An object with two-character keys and numeric values. Keys correspond to the SAM activity categories:

| Key | Description |
|-----|-------------|
| `fb` | Feedbacks |
| `ms` | Mail sent |
| `sm` | Sysop mail |
| `po` | Posts |
| `re` | Responses |
| `gc` | Group changes |
| `pf` | Pfiles |
| `nu` | New users |
| `uf` | Upload files |
| `uk` | Upload KB |
| `df` | Download files |
| `dk` | Download KB |
| `mu` | Minutes used |
| `mi` | Minutes idle |
| `ch` | Charges |

Only non-zero counters appear in the SAM object for a given session.

### Notes
- Read-only. File is read under SEM[12] shared lock.
- Maximum read size is 512 KB (same as `log read`). If the file exceeds this, only the tail portion is read.
- Maximum record tracking is 4000 records.
- MCI escape sequences are stripped in-place before parsing.
- Records are delimited by lines consisting of a single `.` character.
- The `--tail` filter is applied after validating records (only records with a parseable header line are counted), so `--tail 10` returns the last 10 real call records.
- The user detail line format is: `Handle, RealName Phone (VerificationStatus) Country`. The phone number is the last space-delimited token before the parenthesis.
- Up to 32 events per record and 15 SAM pairs per record are tracked.
