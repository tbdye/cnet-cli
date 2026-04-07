# File Commands

File area operations for CNet BBS file transfer subboards (MRK_FILE_TXFER). Commands cover listing, viewing, adding, editing, removing, validating, searching, and auditing files in file transfer areas.

All file commands require `cnet.library` to be open and CNet to be running. Subboard data files are loaded via `OneMoreUser`/`OneLessUser` internally.

## `cnet-cli file list`

### Synopsis
```
cnet-cli file list <sub-id|gokey> [--limit N] [--offset N]
```

### Description
Lists file entries in a file transfer subboard. Returns basic metadata for each file item, suitable for directory listings. Items are returned in subboard order (0-based internal index, exposed as 1-based `index`).

This is a read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number (0-based) or GO key (SubDirName, case-insensitive) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--limit` | integer | 0 (no limit) | Maximum number of items to return |
| `--offset` | integer | 0 | Number of items to skip from the beginning |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key (SubDirName) of the subboard, MCI codes stripped |
| `physnum` | number | Physical subboard number |
| `items` | array | Array of file item objects |
| `total` | number | Total number of items in the subboard (regardless of limit/offset) |

Each object in `items`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item ID (ihead.Number, from the subboard's ID sequence) |
| `index` | number | 1-based display index within the subboard |
| `title` | string | Filename (MCI stripped) |
| `size` | number | File size in bytes (from ihead.Size) |
| `downloads` | number | Download count |
| `validated` | boolean | Whether the file has been validated by sysop |
| `finished` | boolean | Whether the upload is complete |
| `described` | boolean | Whether the file has a short description |
| `missing_file` | boolean | Whether the physical file is flagged as missing |
| `by_account` | number | Account number of the uploader |
| `by_handle` | string | Handle of the uploader (MCI stripped) |
| `post_date` | string\|null | Upload date in ISO 8601 format (YYYY-MM-DDTHH:MM:SS), or null if unset |
| `killed` | boolean | Whether the item is marked as killed/deleted |
| `responses` | number | Number of responses/comments on this file |

### Notes
- The subboard must be a file area (MRK_FILE_TXFER). Returns an error if the subboard is a different type.
- Killed items are included in the listing. Filter on `killed` if you want only live items.
- `total` reflects the full item count, not the number returned after limit/offset filtering.

---

## `cnet-cli file show`

### Synopsis
```
cnet-cli file show <sub-id|gokey> <item-number>
```

### Description
Shows full detail for a single file entry, including all metadata fields, credit/accounting values, behavioral flags, dates, short description, long description text, and responses. This is the most comprehensive view of a file item.

This is a read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index within the subboard |

### Output Fields

Top-level object contains a single `item` key:

| Field | Type | Description |
|-------|------|-------------|
| `item` | object | The file item detail object |

Fields within `item`:

**Basic fields:**

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item ID |
| `index` | number | 1-based display index |
| `title` | string | Filename (MCI stripped) |
| `size` | number | File size in bytes |
| `downloads` | number | Download count |
| `by_account` | number | Uploader's account number |
| `by_handle` | string | Uploader's handle |
| `to_handle` | string | Recipient handle (only present if item.ToID != 0 and resolvable) |
| `to_id` | number | Recipient ID (only present if item.ToID != 0 and not resolvable to an account) |
| `post_date` | string\|null | Upload date (ISO 8601) or null |
| `killed` | boolean | Whether the item is killed |
| `responses` | number | Number of responses |

**File-specific flags:**

| Field | Type | Description |
|-------|------|-------------|
| `validated` | boolean | Sysop-validated |
| `finished` | boolean | Upload complete |
| `described` | boolean | Has short description |
| `missing_file` | boolean | Physical file flagged as missing |
| `partition` | number | Disk partition number (0 = default/ZeroPath) |

**Credit/accounting fields:**

| Field | Type | Description |
|-------|------|-------------|
| `byte_charges` | number | Byte credit charges for downloading |
| `file_charges` | number | File credit charges for downloading |
| `byte_download` | number | Byte download credit cost |
| `file_download` | number | File download credit cost |
| `file_payback` | number | File credit payback to uploader on download |
| `byte_payback` | number | Byte credit payback to uploader on download |
| `byte_rewards` | number | Byte credit rewards |
| `file_rewards` | number | File credit rewards |
| `best_cps` | number | Best characters-per-second transfer rate recorded |

**Behavioral flags:**

| Field | Type | Description |
|-------|------|-------------|
| `private` | boolean | Private file (restricted access) |
| `dl_notify` | boolean | Notify uploader on download |
| `frozen` | boolean | File is frozen (cannot be purged) |
| `free` | boolean | Free download (no credit charge) |
| `favorite` | boolean | Marked as favorite |
| `transformed` | boolean | File has been transformed/converted |
| `purge_kill` | boolean | Purge behavior flag |
| `integrity` | number | File integrity check value |
| `auto_grab` | boolean | Auto-grab flag |
| `purge_status` | number | Purge status (0-4) |
| `virus_checked` | boolean | Virus scan completed |
| `override` | boolean | Override flag |

**Dates:**

| Field | Type | Description |
|-------|------|-------------|
| `show_date` | string\|null | Display date (ISO 8601) or null |
| `used_date` | string\|null | Last download date (ISO 8601) or null |

**Short description (_Short file):**

| Field | Type | Description |
|-------|------|-------------|
| `info_x` | number | Offset into the _Short file |
| `info_len` | number | Length of the short description |
| `short_desc` | string\|null | Short description text, or null if unavailable |

**HeaderType metadata (present only if _text contains a HeaderType record):**

| Field | Type | Description |
|-------|------|-------------|
| `by_name` | string | Author's real name from HeaderType (only if non-empty) |
| `by_user` | string | Author's handle from HeaderType (only if non-empty) |
| `to_name` | string | Recipient's real name from HeaderType (only if ToID != 0 and non-empty) |
| `to_user` | string | Recipient's handle from HeaderType (only if ToID != 0 and non-empty) |
| `post_date_header` | string | Post date from the HeaderType record (ISO 8601, only if non-null) |
| `org` | string | Organization field from HeaderType (only if non-empty) |

**Long description:**

| Field | Type | Description |
|-------|------|-------------|
| `text` | string\|null | Long description text, or null if none |

**Responses:**

| Field | Type | Description |
|-------|------|-------------|
| `responses_list` | array | Array of response objects |

Each response object:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Response unique ID |
| `by_handle` | string\|null | Response author's handle (null if not resolvable) |
| `by_id` | number | Response author's ID (only if handle not resolvable) |
| `text` | string\|null | Response text, or null |
| `by_name` | string | Author real name from HeaderType (conditional) |
| `by_user` | string | Author handle from HeaderType (conditional) |
| `to_name` | string | Recipient real name from HeaderType (conditional) |
| `to_user` | string | Recipient handle from HeaderType (conditional) |
| `org` | string | Organization from HeaderType (conditional) |
| `post_date` | string\|null | Response date (ISO 8601) or null |

### Notes
- Item number is 1-based (first item is 1, not 0).
- The `_Message3` responses file is read directly from disk since `OneMoreUser` does not load it.
- HeaderType metadata fields are only present when the _text entry uses the HeaderType format (magic value 0xBB25B8C4). Legacy plain-text entries omit these fields.

---

## `cnet-cli file add`

### Synopsis
```
cnet-cli file add <sub-id|gokey> --title <filename> --author <account> [--desc <description>]
```

### Description
Adds a new file entry to a file transfer subboard. The physical file must already exist on disk at the expected path (partition 0). The command verifies the file exists and records its size. Files added this way are automatically marked as validated and finished.

This is a write operation. It modifies the subboard's item/header data and persists changes to disk.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--title` | string | (required) | Filename of the physical file on disk |
| `--author` | integer | (required) | Account number of the uploader |
| `--desc` | string | (none) | Short description text. **Currently ignored** with a warning; _Short file format is unverified |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"added"` on success |
| `physnum` | number | Physical subboard number |
| `item_count` | number | New total item count in the subboard |
| `next_id` | number | Next available unique ID (sub->count) |
| `title` | string | Filename as stored |
| `size` | number | File size in bytes as detected on disk |
| `by_account` | number | Author account number |
| `warnings` | array | Array of warning strings (present if warnings were generated, e.g., --desc ignored) |

### Notes
- The physical file must exist at `UDBase0:<gokey>/<filename>` (or `ZeroPath/<filename>` if the subboard has a custom ZeroPath). If the file is not found, the command fails with an error showing the expected path.
- `--author` must be a valid, existing account number.
- Added files are pre-set with: `Validated=1`, `Finished=1`, `Described=0`, `PurgeKill=1`. Default values for `PurgeStatus`, `DLnotifyULer`, and `override` are inherited from the subboard configuration.
- `--desc` is accepted syntactically but currently has no effect. A warning is emitted explaining this.
- Requires exclusive semaphore on the subboard during the add operation.

---

## `cnet-cli file edit`

### Synopsis
```
cnet-cli file edit <sub-id|gokey> <item-number> [--validated 0|1] [--frozen 0|1] [--free 0|1] [--private 0|1] [--missing 0|1] [--purge-status N] [--desc "..."]
```

### Description
Edits flags on an existing file entry. Only the specified flags are changed; all others remain unchanged. At least one flag must be specified (unless `--desc` is the only flag, in which case a no-change result is returned since `--desc` is currently ignored).

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--validated` | 0 or 1 | (unchanged) | Set validated flag |
| `--frozen` | 0 or 1 | (unchanged) | Set frozen flag |
| `--free` | 0 or 1 | (unchanged) | Set free download flag |
| `--private` | 0 or 1 | (unchanged) | Set private flag |
| `--missing` | 0 or 1 | (unchanged) | Set missing file flag |
| `--purge-status` | 0-4 | (unchanged) | Set purge status (must be 0-4, validated) |
| `--desc` | string | (none) | Short description text. **Currently ignored** with a warning; _Short file format is unverified |

### Output Fields

On successful edit:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"edited"` on success, or `"no_change"` if only --desc was passed |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Filename (MCI stripped) |
| `validated` | boolean | Current validated state |
| `frozen` | boolean | Current frozen state |
| `free` | boolean | Current free state |
| `private` | boolean | Current private state |
| `missing_file` | boolean | Current missing file flag state |
| `purge_status` | number | Current purge status value |
| `warnings` | array | Array of warning strings (present if warnings were generated) |

On no-change (only `--desc` was passed):

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"no_change"` |
| `note` | string | Explanation that --desc was skipped |

### Notes
- `--purge-status` is range-validated: values outside 0-4 are rejected before any subboard access.
- If no recognized flags are provided (and `--desc` is not passed), returns error "No fields to change".
- The item is read, modified in memory, and written back via `ZPutItem`.

---

## `cnet-cli file remove`

### Synopsis
```
cnet-cli file remove <sub-id|gokey> <item-number> [--delete-physical]
```

### Description
Marks a file entry as killed. Optionally deletes the physical file from disk. The item remains in the subboard data but is flagged with `Killed=1`.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--delete-physical` | flag | off | Also delete the physical file from the Amiga filesystem |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"removed"` on success |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Filename (MCI stripped) |
| `file_deleted` | boolean | Whether the physical file was deleted (only present when `--delete-physical` is used) |
| `warnings` | array | Array of warning strings (present if physical deletion failed) |

### Notes
- Returns an error if the item is already killed.
- When `--delete-physical` is used and the physical file cannot be deleted, the item is still marked as killed but a warning is emitted.
- The physical file path is constructed from the subboard's ZeroPath or UDBase partition, the GO key, and the filename.

---

## `cnet-cli file validate`

### Synopsis
```
cnet-cli file validate <sub-id|gokey> <item-range>
```

### Description
Validates one or more file entries in a file transfer subboard. Sets `Validated=1` on each item in the specified range that is not already validated and not killed. This is a batch operation.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-range>` | Yes | Range specifier (see below) |

### Range Syntax

The range argument supports several formats:

| Format | Example | Description |
|--------|---------|-------------|
| Single number | `5` | Validate item 5 only |
| Hyphenated range | `3-10` | Validate items 3 through 10 |
| Comma-separated | `1,3,5-10` | Validate items 1, 3, and 5 through 10 (requires cnet4.library) |
| Keyword | `all` | Validate all items in the subboard |

Range values are 1-based item indices. Values are clamped to the valid range (1 to total item count). The comma-separated format requires `cnet4.library` to be available; without it, only single numbers and `N-M` ranges are supported.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"validated"` on success |
| `physnum` | number | Physical subboard number |
| `validated` | number | Number of items actually validated (changed from unvalidated to validated) |
| `total_in_range` | number | Total number of items processed in the range |
| `range_start` | number | First item index in the processed range |
| `range_end` | number | Last item index in the processed range |

### Notes
- Items that are already validated or killed are skipped (counted in `total_in_range` but not in `validated`).
- The `all` keyword is handled as a fast path, iterating sequentially from 1 to total count.
- Invalid range strings return an error "Invalid range: expected number, range (N-M), comma-separated, or 'all'".

---

## `cnet-cli file find`

### Synopsis
```
cnet-cli file find <query> [--sub <sub-id|gokey>] [--limit N] [--field filename|description|uploader]
```

### Description
Searches for files across one or all file transfer subboards. The search is case-insensitive substring matching. By default, searches filenames in all non-killed, non-subdirectory file areas.

This is a read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<query>` | Yes | Search string (case-insensitive substring match) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--sub` | string | (all file areas) | Restrict search to a specific subboard (by ID or GO key) |
| `--limit` | integer | 100 | Maximum number of matches to return |
| `--field` | string | `filename` | Which field to search: `filename`, `description`, or `uploader` |

### Field Search Behavior

| Field | Searches |
|-------|----------|
| `filename` | The `Title` field of each item (the filename) |
| `description` | The short description text read from the `_Short` file (items without a description are skipped) |
| `uploader` | The handle of the uploading user (resolved from `ByAccount`) |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `query` | string | The search query string |
| `field` | string | The field searched (`"filename"`, `"description"`, or `"uploader"`) |
| `limit` | number | The limit value used |
| `matches` | array | Array of matching file item objects |
| `skipped` | array | Array of physical subboard numbers that could not be loaded |
| `subboards_searched` | number | Number of subboards successfully searched |
| `total_matches` | number | Total number of matches found |

Each object in `matches`:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number containing the match |
| `subboard` | string | GO key of the subboard |
| `item_index` | number | 1-based item index within the subboard |
| `item_number` | number | Unique item ID |
| `title` | string | Filename (MCI stripped) |
| `size` | number | File size in bytes |
| `downloads` | number | Download count |
| `validated` | boolean | Validated flag |
| `by_account` | number | Uploader's account number |
| `by_handle` | string | Uploader's handle |
| `post_date` | string\|null | Upload date (ISO 8601) or null |

### Notes
- When `--sub` is omitted, searches all non-killed, non-subdirectory file areas (up to 256 subboards).
- Killed items within subboards are skipped.
- If `--limit` is <= 0, it defaults to 100.
- Subboards that fail to load via `OneMoreUser` are listed in the `skipped` array and not counted in `subboards_searched`.

---

## `cnet-cli file missing`

### Synopsis
```
cnet-cli file missing [<sub-id|gokey>] [--update]
```

### Description
Audits file transfer subboards for missing physical files. Performs two passes: first identifies files whose physical file is missing from disk, then identifies files flagged as missing whose physical file has been restored. With `--update`, sets or clears the `MissingFile` flag accordingly.

This is a read-only operation by default. With `--update`, it becomes a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | No | Restrict audit to a specific subboard. If omitted, audits all file areas |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--update` | flag | off | Actually update the MissingFile flag on items (set to 1 for missing files, clear to 0 for restored files) |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `missing` | array | Array of items whose physical file is missing from disk |
| `restored` | array | Array of items flagged as missing but whose physical file now exists |
| `summary` | object | Summary statistics |

Each object in `missing`:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key of the subboard |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Filename |
| `size` | number | Expected file size in bytes |
| `partition` | number | Disk partition number |
| `expected_path` | string | Full AmigaOS path where the file was expected |
| `by_account` | number | Uploader's account number |
| `by_handle` | string | Uploader's handle |
| `missing_file_flag` | boolean | Current state of the MissingFile flag before any update |
| `updated` | boolean | Whether the flag was changed (only present when `--update` is used) |

Each object in `restored`:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key of the subboard |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Filename |
| `size` | number | File size in bytes |
| `partition` | number | Disk partition number |
| `expected_path` | string | Full AmigaOS path where the file was found |
| `missing_file_flag` | boolean | Always `true` (these items were flagged as missing) |
| `updated` | boolean | Whether the flag was cleared (only present when `--update` is used) |

The `summary` object:

| Field | Type | Description |
|-------|------|-------------|
| `subboards_scanned` | number | Number of subboards successfully scanned |
| `items_scanned` | number | Total number of file items checked |
| `missing_count` | number | Number of files missing from disk |
| `restored_count` | number | Number of files restored (flagged missing but file exists) |
| `updated_count` | number | Number of items whose MissingFile flag was changed (0 if `--update` not used) |
| `skipped_subboards` | array | Array of physical subboard numbers that could not be loaded |

### Notes
- Only finished files with a non-zero size are checked. Unfinished uploads and zero-size items are skipped. Killed items are also skipped.
- Without `--update`, this is a dry-run audit. The `updated` field will not be present in the output.
- With `--update`, items in the `missing` array that do not already have `MissingFile=1` are updated (flag set to 1). Items in the `restored` array have `MissingFile` cleared to 0.
- The `updated` field in each item indicates whether a change was actually made. Items already in the correct state show `updated: false` (for missing) or are not present (for restored, which always update).
- When no subboard is specified, scans all non-killed, non-subdirectory file areas (up to 256 subboards).
- File existence is verified using `Lock`/`Examine` (not CNet's `FileSize`) to distinguish missing files from empty files.
