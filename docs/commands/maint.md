# Maintenance Commands

Commands for rebuilding indexes, recounting data, and compacting storage files. These are write operations (except in dry-run mode) that modify on-disk data structures. Use with care.

## `cnet-cli maint pointers`

### Synopsis
```
cnet-cli maint pointers
```

### Description
Rebuilds the user index files from the in-memory Key[] array. Reconstructs the sorted IName[] and IPhone[] index arrays, then writes all index files to disk. This is the equivalent of the CNet "rebuild pointers" maintenance operation.

This is a **write operation**. It always applies changes immediately (there is no dry-run mode).

### Arguments
None.

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `command` | string | Always `"maint_pointers"` |
| `accounts` | number | Total account count (`Nums[NUMS_CURRENT_ACCOUNTS]`) |
| `iname_entries` | number or null | Number of entries in the rebuilt IName[] index, or null if rebuild failed |
| `iphone_entries` | number or null | Number of entries in the rebuilt IPhone[] index, or null if rebuild failed |
| `files_written` | array of string | List of files successfully written |
| `warnings` | array of string | Warnings about failures |

**Possible `files_written` values:**

| File | Description |
|------|-------------|
| `bbs.ukeys4` | Raw Key[] dump |
| `bbs.uind1` | IName[] sorted by handle/realname |
| `bbs.uind2` | IPhone[] sorted by phone number |
| `bbs.sdata` | Nums[] array (5 longs) |

### Notes
- **Write operation.** Acquires SEM[1] exclusive for the entire duration.
- Idempotent and non-destructive to primary user data. The index files are derived entirely from the in-memory Key[] array.
- If an index rebuild fails (out of memory), the corresponding entry is null and a warning is emitted. File writes are skipped if either index rebuild fails.
- Individual file write failures are reported as warnings.

---

## `cnet-cli maint count`

### Synopsis
```
cnet-cli maint count [--apply] [--dry-run] [--sub <id|gokey>] [--subs-only] [--nums-only]
```

### Description
Recounts subboard item/response counts and system account counters by reading the actual data files on disk. Compares ground-truth values against the in-memory state and reports discrepancies.

Default mode is **dry-run** (report only). Use `--apply` to write corrected values to memory and disk.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--apply` | flag | off | Write corrected values to memory and disk |
| `--dry-run` | flag | on (default) | Report differences without writing (explicit, same as default) |
| `--sub` | string | -- | Recount only a single subboard (by physical number or GO key). Implies `--subs-only`. |
| `--subs-only` | flag | off | Skip Phase B (Nums[] recount), only do subboard recount |
| `--nums-only` | flag | off | Skip Phase A (subboard recount), only do Nums[] recount |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `command` | string | Always `"maint_count"` |
| `mode` | string | `"apply"` or `"dry-run"` |
| `changes` | array | Per-subboard change details (Phase A, absent if `--nums-only`) |
| `subboards_scanned` | number | Number of subboards examined (absent if `--nums-only`) |
| `subboards_skipped` | number | Number of subboards skipped (killed, subdirectories, or errors) (absent if `--nums-only`) |
| `subboards_changed` | number | Number of subboards with discrepancies (absent if `--nums-only`) |
| `nums` | object | Nums[] comparison (Phase B, absent if `--subs-only`) |
| `warnings` | array of string | Non-fatal warnings. Present only if warnings exist. |

Each element of `changes` (only for subboards with discrepancies):

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number |
| `go_key` | string | Subboard GO key (SubDirName) |
| `title` | string | Subboard title |
| `rn` | object | Item record count: `{"old": N, "new": N}` |
| `nm` | object | Message record count: `{"old": N, "new": N}` |
| `count` | object | Next unique ID counter: `{"old": N, "new": N}` |
| `nNewMess` | object | New message buffer: `{"old": N, "new": 0}` (always cleared to 0) |
| `responses_fixed` | number | Number of per-item Responses values corrected |

`nums` object (Phase B):

| Field | Type | Description |
|-------|------|-------------|
| `current_accounts` | object | `{"old": N, "new": N, "changed": bool}` -- from bbs.udata4 file size |
| `inuse_accounts` | object | `{"old": N, "new": N, "changed": bool}` -- from Key[] scan |
| `high_id` | object | `{"old": N, "new": N, "changed": bool}` -- max IDNumber in Key[] |

### Notes
- **Write operation when `--apply` is used.** Dry-run by default.
- `--subs-only` and `--nums-only` are mutually exclusive.
- Phase A (subboard recount) reads _Items3, _Headers3, and _Message3 directly from disk without using OneMoreUser/OneLessUser. It computes:
  - `rn` from _Items3 file size divided by record size
  - `nm` from _Message3 file size divided by record size
  - `count` from max(ItemHeader.Number, Message.Number) + 1 (never decreased)
  - `nNewMess` unconditionally cleared to 0
  - Per-item `Responses` from _Message3 scan
- Phase B scans the in-memory Key[] array under SEM[1] shared to recount active accounts and highest ID. Total accounts comes from the bbs.udata4 file size.
- Safety: `count` is never decreased from its current value. A warning is emitted if count would jump by more than 10x (suspicious data).
- When `--apply` is used:
  - Phase A writes corrected subboard records under SEM[5] exclusive, and corrected _Headers3 under the subboard's semaphore exclusive (only if Responses changed).
  - Phase B writes corrected Nums[] under SEM[1] + SEM[4] exclusive, and writes bbs.sdata to disk.
- Killed subboards and subdirectory entries are skipped.
- SEM[5] is not held during per-subboard file I/O (accepted TOCTOU precedent consistent with message.c).

---

## `cnet-cli maint repair-mail`

### Synopsis
```
cnet-cli maint repair-mail <account|handle> [--folder <name>] [--apply]
cnet-cli maint repair-mail --all [--apply]
```

### Description
Compacts mail data files by removing dead text from `_mtext4` and updating seek offsets in `_mhead4`. Also zeroes the `unknown_0` field (offset +0) in all mail header records, fixing a bug where non-zero values were erroneously written.

Default mode is **dry-run** (report only). Use `--apply` to write compacted files.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<account\|handle>` | Yes (unless `--all`) | User account number or handle to repair |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--apply` | flag | off | Write compacted files to disk |
| `--dry-run` | flag | on (default) | Report without writing (explicit, same as default) |
| `--all` | flag | off | Process all users by enumerating `mail:users/` |
| `--folder` | string | -- | Process only the named folder (requires a specific user, not `--all`) |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `command` | string | Always `"maint_repair_mail"` |
| `mode` | string | `"apply"` or `"dry-run"` |
| `users` | array | Per-user compaction results |
| `users_scanned` | number | Number of users processed |
| `users_compacted` | number | Number of users with at least one folder compacted |
| `folders_scanned` | number | Total folders examined across all users |
| `folders_compacted` | number | Total folders with reclaimed space or bug fixes |
| `total_bytes_reclaimed` | number | Total bytes of dead text removed across all folders |
| `warnings` | array of string | Non-fatal warnings. Present only if warnings exist. |

Each element of `users`:

| Field | Type | Description |
|-------|------|-------------|
| `uucp` | string | User's UUCP name |
| `account` | number | Account number |
| `folders` | array | Per-folder compaction results |

Each element of `folders` (only for folders with activity):

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Folder name |
| `records` | number | Number of mail header records in the folder |
| `mtext4_old_size` | number | Original _mtext4 file size in bytes |
| `mtext4_new_size` | number | Compacted _mtext4 size in bytes |
| `bytes_reclaimed` | number | Bytes saved by compaction |
| `bug3_records_fixed` | number | Number of records where unknown_0 was zeroed |

### Notes
- **Write operation when `--apply` is used.** Dry-run by default.
- Requires cnetmail.library (for `GetMailSems()` and `CreateFolderName()`). Returns an error if unavailable.
- Semaphore protocol: per-user mail semaphore (`GetMailSems()[account-1]`) held exclusive for the duration of that user's folder processing. SEM[1] shared for Key[] UUCP lookup.
- The compaction writes use a safe rename sequence: write `.new` files, rename originals to `.old`, rename `.new` to final names. If any step fails, the operation rolls back. Stale `.old` files from interrupted previous runs are cleaned up.
- Orphaned `_mtext4` files (those without a corresponding `_mhead4`) are truncated during compaction.
- In `--all` mode, user directories under `mail:users/` that don't match any account produce an "orphaned mail dir" warning.
- On-disk mail header records are 810 bytes each. Key field offsets: unknown_0 at +0 (ULONG), length at +474 (USHORT), seek at +800 (ULONG). All are big-endian.
- Cannot specify both a user argument and `--all`.
- Cannot use `--folder` with `--all`.

---

## `cnet-cli maint repair-sub`

### Synopsis
```
cnet-cli maint repair-sub <id|gokey> --apply
```

### Description
Subboard text pool compaction. **Not yet implemented.** Returns an error message explaining the limitation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<id\|gokey>` | Yes | Subboard physical number or GO key |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--apply` | flag | -- | Would trigger the compaction (currently unused) |

### Output Fields

Always returns an error:

| Field | Type | Description |
|-------|------|-------------|
| `error` | string | `"maint repair-sub is not yet available (OneMoreUser heap corruption)"` |

### Notes
- **Not yet implemented.** The underlying CNet `OneMoreUser()` function corrupts the libnix malloc heap, making all subsequent memory allocation and stdio operations deadlock. A different implementation approach is needed (direct file I/O without OneMoreUser, or a native SAS/C-compiled helper).
- This command is registered in the dispatch table and accepts arguments, but always returns an error regardless of input.
