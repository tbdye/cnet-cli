# Subboard Commands

Subboard commands manage the CNet BBS subboard hierarchy -- message bases, file transfer areas, doors, and organizational subdirectories. All subboard data is stored in the `SysData:subboards4` binary file (696-byte `SubboardType4` records) and mirrored in CNet's shared-memory `MainPort->Subboard[]` array.

## Subboard Resolution

All commands that accept a `<sub-id|gokey>` argument resolve the subboard through `resolve_subboard()`, which tries three strategies in order:

1. **Numeric**: If the argument is all digits, it is treated as a physical subboard number (0-based index into the subboard array). Valid range: 0 to `myp->ns - 1`.
2. **GO key**: Case-insensitive match against `SubDirName` (the GO key) of all non-killed subboards. Acquires SEM[5] shared for the search.
3. **NumFromUnique()**: Falls back to the CNet library function `NumFromUnique()` for any other string identifier.

If none of these produce a valid subboard number, the command returns an error.

## Concurrency

- **Read commands** (`list`, `show`, `tree`, `path`, `disk-usage`) acquire SEM[5] shared.
- **Mutation commands** (`create`, `edit`, `delete`) acquire SEM[5] exclusive and perform dual writes: the in-memory struct is updated first, then the corresponding record in `SysData:subboards4` is written to disk.

---

## `cnet-cli sub list`

### Synopsis
```
cnet-cli sub list [--active] [--type msg|file|door]
```

### Description
Lists all subboards in the system. Returns summary information for each subboard. By default includes killed (deleted) subboards; use `--active` to exclude them.

This is a read-only command.

### Arguments
None (positional).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--active` | flag | off | Exclude killed (deleted) subboards from the listing |
| `--type` | string | (none) | Filter by subboard type: `msg` (MsgBase, marker 0), `file` (FileTxfer, marker 1), or `door` (all door types, markers 3-9) |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `subboards` | array | Array of subboard summary objects |

Each element of `subboards`:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number (0-based index) |
| `title` | string | Subboard title (MCI codes stripped) |
| `go_key` | string | GO key / SubDirName (MCI codes stripped) |
| `marker` | number | Marker base type (0=MsgBase, 1=FileTxfer, 3=TextDoor, 4=TextFile, 5=CNetCDoor, 6=ARexxDoor, 7=ADosDoor, 8=BBSMacro, 9=DirCommander) |
| `marker_name` | string | Human-readable marker type name |
| `killed` | boolean | Whether the subboard is marked as deleted |
| `root` | boolean | Whether this is the root subboard |
| `parent` | number | Physical number of the parent subboard |
| `child` | number | Physical number of the first child subboard (-1 if none) |
| `next` | number | Physical number of the next sibling subboard (-1 if none) |
| `data_path` | string | AmigaOS path to the subboard's data directory (MCI stripped) |
| `users` | number | Number of users currently accessing this subboard |
| `next_id` | number | Next unique ID sequence number (for items and responses) |
| `item_count` | number | Number of items (messages or files) in this subboard |
| `access` | string | Access bitmask as hex string (e.g., `"0xffffffff"`) |
| `serial` | number | Subboard serial number |

### Notes
- The `marker` field contains only the base type bits; the killed bit is reported separately.
- `next_id` is the cumulative unique ID counter, not the item count. It increments for both items and responses.

---

## `cnet-cli sub show`

### Synopsis
```
cnet-cli sub show <sub-id|gokey>
```

### Description
Displays full detail for a single subboard, including all configuration fields, access restrictions, boolean flags, sub-operator assignments, and activity dates.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Subboard identifier: physical number, GO key, or unique name |

### Options
None.

### Output Fields

The output includes all summary fields from `sub list` (see above), plus the following detail fields:

**Configuration fields:**

| Field | Type | Description |
|-------|------|-------------|
| `subdirectory` | boolean | Whether this is a subdirectory (organizational node, not a content area) |
| `closed` | boolean | Whether the subboard is closed to new posts |
| `max_items` | number | Maximum number of items allowed |

**Access control fields:**

| Field | Type | Description |
|-------|------|-------------|
| `access_groups` | string | Read access as human-readable group string (e.g., `"1-3,5,7-10"`) |
| `post_access` | string | Post access bitmask as hex |
| `post_access_groups` | string | Post access as human-readable group string |
| `respond_access` | string | Respond access bitmask as hex |
| `respond_access_groups` | string | Respond access as human-readable group string |
| `upload_access` | string | Upload access bitmask as hex |
| `upload_access_groups` | string | Upload access as human-readable group string |
| `download_access` | string | Download access bitmask as hex |
| `download_access_groups` | string | Download access as human-readable group string |

**Content flags:**

| Field | Type | Description |
|-------|------|-------------|
| `real_names` | boolean | Require real names instead of handles |
| `anonymous` | boolean | Allow anonymous posting |
| `private_area` | boolean | Private area flag |
| `no_mci` | boolean | Disable MCI code processing |

**Type/format fields:**

| Field | Type | Description |
|-------|------|-------------|
| `computer_types` | string | Allowed computer types bitmask as hex |
| `oldest` | number | Maximum user age in years |
| `arcs` | string | Allowed archive types bitmask as hex |
| `subvalid` | number | Validity sentinel (should be 1234567890 for valid subboards) |

**Sub-operator fields:**

| Field | Type | Description |
|-------|------|-------------|
| `subop_ids` | array | Array of 6 sub-operator ID numbers |
| `subop_accs` | array | Array of 6 sub-operator account numbers |

**Hour/access restriction fields:**

| Field | Type | Description |
|-------|------|-------------|
| `hours` | string | Hour-slot bitmask as hex (bits 0-23, NOT access groups) |
| `baud_hours` | string | Baud rate hour restriction bitmask as hex |
| `hour_access` | string | Hour access group bitmask as hex |
| `hour_access_groups` | string | Hour access as human-readable group string |
| `hour_union_flags` | string | Hour union flags bitmask as hex |
| `hour_union_flags_groups` | string | Hour union flags as human-readable group string |
| `union_flags` | string | Union flags bitmask as hex |
| `union_flags_groups` | string | Union flags as human-readable group string |
| `baud` | number | Minimum baud rate |
| `gender` | string | Gender restriction: `"any"`, `"M"`, or `"F"` |
| `youngest` | number | Minimum age in years |
| `inactive_days` | number | Auto-remove after N inactive days |
| `free_days` | number | Free trial days |
| `min_free_bytes` | number | Minimum free disk bytes required |
| `time_credit` | number | Time credit value (0-100) |

**Boolean configuration flags:**

| Field | Type | Description |
|-------|------|-------------|
| `verification` | boolean | Require file verification |
| `dup_check` | boolean | Enable duplicate checking |
| `show_unvalidated` | boolean | Show unvalidated items |
| `no_signatures` | boolean | Disable signatures |
| `no_read_charges` | boolean | Disable read charges |
| `no_write_charges` | boolean | Disable write charges |
| `invitation` | boolean | Invitation-only subboard |
| `user_must_join` | boolean | Users must join before accessing |
| `delete_own` | boolean | Users can delete their own posts |
| `carbon_copy` | boolean | Enable carbon copy |
| `cdrom` | boolean | CD-ROM mode (read-only media) |
| `qwk_replies` | boolean | Allow QWK offline reader replies |
| `persist` | boolean | Persist subboard data in memory |
| `delay` | boolean | Delay processing flag |
| `diz_save` | boolean | Save FILE_ID.DIZ descriptions |

**Obits (option bits) fields:**

| Field | Type | Description |
|-------|------|-------------|
| `obits` | string | Raw obits bitmask as hex |
| `obit_showbows` | boolean | Show bows (OFF_SHOWBOWS flag) |
| `obit_diz_alnum` | boolean | DIZ alphanumeric filter (OFF_DIZALNUM) |
| `obit_diz_strip_chars` | boolean | DIZ strip special characters (OFF_DIZSTRIPCHARS) |
| `obit_diz_strip_text` | boolean | DIZ strip text (OFF_DIZSTRIPTEXT) |
| `obit_diz_strip_cr` | boolean | DIZ strip carriage returns (OFF_DIZSTRIPCR) |

**Activity date fields:**

| Field | Type | Description |
|-------|------|-------------|
| `last_upload` | string or null | Last upload date as ISO 8601 string, or null if never |
| `last_message` | string or null | Last message date as ISO 8601 string, or null if never |

### Notes
- Dates are formatted as `"YYYY-MM-DDTHH:MM:SS"` (no timezone, local time).
- The `access` field (hex) from the summary and the `access_groups` field (human-readable) from the detail represent the same bitmask in different formats.
- `hours` and `baud_hours` are hour-slot bitmasks (bits 0-23), NOT access group bitmasks. They must not be interpreted through `ExpandFlags`.

---

## `cnet-cli sub tree`

### Synopsis
```
cnet-cli sub tree
```

### Description
Outputs the full subboard hierarchy as a recursive tree structure. Starts from the root subboard and follows Child/Next pointers. Uses a visited bitmap to prevent infinite loops from corrupted pointers.

This is a read-only command.

### Arguments
None.

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `tree` | array | Array of top-level tree nodes (usually one: the root) |

Each tree node:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number |
| `title` | string | Subboard title (MCI stripped) |
| `go_key` | string | GO key (MCI stripped) |
| `marker_name` | string | Human-readable type name |
| `root` | boolean | Whether this is the root subboard |
| `subdirectory` | boolean | Whether this is a subdirectory node |
| `children` | array | Array of child tree nodes (recursive structure, same fields) |

### Notes
- The `children` array is always present, even if empty.
- Maximum 4096 subboards are supported in the traversal (limited by the visited bitmap).
- If the root subboard has siblings at the top level (via its Next pointer), they are emitted as additional top-level entries in the `tree` array.

---

## `cnet-cli sub path`

### Synopsis
```
cnet-cli sub path <sub-id|gokey>
```

### Description
Returns the ancestry path from a given subboard up to the root. Walks the Parent chain and outputs each node along the way in leaf-to-root order.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Subboard identifier |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical number of the queried subboard |
| `title` | string | Title of the queried subboard |
| `go_key` | string | GO key of the queried subboard |
| `depth` | number | Number of nodes in the path (including the queried node) |
| `path` | array | Array of path entries from leaf to root |

Each element of `path`:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number |
| `title` | string | Subboard title (MCI stripped) |
| `go_key` | string | GO key (MCI stripped) |

### Notes
- The first element of `path` is the queried subboard itself; the last element is the root.
- Maximum depth is 64. Deeper hierarchies are truncated.
- Traversal stops when it reaches the root (Parent is self, negative, or out of range).

---

## `cnet-cli sub disk-usage`

### Synopsis
```
cnet-cli sub disk-usage <sub-id|gokey>
```

### Description
Reports the total disk usage (in bytes) for a subboard's data directory. Uses the CNet library function `DirectorySize()` to recursively total all file sizes.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Subboard identifier |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number |
| `title` | string | Subboard title (MCI stripped) |
| `go_key` | string | GO key (MCI stripped) |
| `data_path` | string | AmigaOS path to the subboard's data directory |
| `bytes` | number | Total disk usage in bytes |

### Notes
- Requires `cnet.library` to be available (for `DirectorySize()` and `BuildDir()`).
- Returns an error if the subboard has no data path set.

---

## `cnet-cli sub create`

### Synopsis
```
cnet-cli sub create --title <title> --go <gokey> --type <type> --parent <sub-id|gokey>
    [--data-path <path>] [--access <hex|groups>] [--max-items N]
```

### Description
Creates a new subboard and inserts it into the hierarchy as a child of the specified parent. Finds a free slot by scanning for killed entries first, then extends the array if room remains (up to the pre-allocated `gc.nsub` limit). Creates the data directory and initializes empty data files (_Items3, _Headers3, _Free) for MsgBase and FileTxfer types.

This is a write operation. Acquires SEM[5] exclusive.

### Arguments
None (all parameters are flags).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--title` | string | (required) | Subboard title |
| `--go` | string | (required) | GO key (SubDirName) |
| `--type` | string | (required) | Subboard type (see values below) |
| `--parent` | string | (required) | Parent subboard identifier (number, GO key, or unique name) |
| `--data-path` | string | derived from parent | AmigaOS path to the data directory. If omitted, derived as `{parent_data_path}/{gokey}/` |
| `--access` | string | `0xffffffff` (all groups) | Access bitmask. Accepts hex (`0xNNNNNNNN`) or group string (`1-3,5,7-10`). Applied to all five access fields (read, post, respond, upload, download). |
| `--max-items` | number | 500 | Maximum number of items. Must be 1 to 4000. |

**Supported `--type` values:**

| Value | Marker | Description |
|-------|--------|-------------|
| `msg` | 0 (MRK_MSG_BASE) | Message base |
| `file` | 1 (MRK_FILE_TXFER) | File transfer area |
| `subdir` | 0 (MRK_MSG_BASE + Subdirectory=1) | Organizational subdirectory |
| `textdoor` | 3 (MRK_TEXT_DOOR) | Text door |
| `textfile` | 4 (MRK_TEXT_FILE) | Text file |
| `cdoor` | 5 (MRK_CNETC_DOOR) | CNet C door |
| `arexx` | 6 (MRK_AREXX_DOOR) | ARexx door |
| `ados` | 7 (MRK_ADOS_DOOR) | AmigaDOS door |
| `macro` | 8 (MRK_BBS_MACRO) | BBS macro |
| `dircom` | 9 (MRK_DIRECT_COMMANDER) | Directory Commander |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"created"` |
| `physnum` | number | Physical number assigned to the new subboard |

Plus all summary fields (see `sub list` output).

### Notes
- Default access-control values are permissive: ComputerTypes=0xFFFFFFFF (all), Oldest=99, Arcs=0xFFFFFFFF (all).
- The serial number comes from `gc2.nextsubser` and is incremented in memory only. CNet saves the config periodically; a crash before save could result in duplicate serial numbers.
- The subboard's `sem` pointer (allocated by CNet at boot) is preserved across the `memset` initialization to prevent hangs in `OneMoreUser`/`OneLessUser`.
- Dual writes: the new subboard, its parent, and any modified sibling are all written to disk.

---

## `cnet-cli sub edit`

### Synopsis
```
cnet-cli sub edit <sub-id|gokey> [--flag value] ...
```

### Description
Modifies one or more fields of an existing subboard. At least one field must be changed; providing no flags is an error. Changes are written to both the in-memory struct and the disk file atomically under SEM[5] exclusive lock.

This is a write operation. Acquires SEM[5] exclusive.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Subboard identifier |

### Options

**String/path fields:**

| Option | Type | Description |
|--------|------|-------------|
| `--title` | string | Subboard title |
| `--go` | string | GO key (SubDirName) |
| `--type` | string | Subboard type (same values as `sub create --type`). Preserves the killed bit. |
| `--data-path` | string | AmigaOS data directory path |

**Access control fields** (accept hex `0xNNNNNNNN` or group string `1-3,5,7-10`):

| Option | Type | Description |
|--------|------|-------------|
| `--access` | hex or groups | Read access bitmask |
| `--post-access` | hex or groups | Post access bitmask |
| `--respond-access` | hex or groups | Respond access bitmask |
| `--upload-access` | hex or groups | Upload access bitmask |
| `--download-access` | hex or groups | Download access bitmask |
| `--hour-access` | hex or groups | Hour access group bitmask |
| `--hour-union-flags` | hex or groups | Hour union flags bitmask |
| `--union-flags` | hex or groups | Union flags bitmask |

**Numeric fields:**

| Option | Type | Range | Description |
|--------|------|-------|-------------|
| `--max-items` | number | 1-4000 | Maximum item count |
| `--oldest` | number | 0-255 | Maximum user age |
| `--baud` | number | >= 0 | Minimum baud rate |
| `--youngest` | number | 0-255 | Minimum user age |
| `--inactive-days` | number | 0-32767 | Auto-remove after N inactive days |
| `--free-days` | number | 0-32767 | Free trial days |
| `--min-free-bytes` | number | >= 0 | Minimum free disk space |
| `--time-credit` | number | 0-100 | Time credit percentage |

**Hex bitmask fields** (accept hex strings only, NOT group strings):

| Option | Type | Description |
|--------|------|-------------|
| `--computer-types` | hex | Allowed computer types bitmask |
| `--arcs` | hex | Allowed archive types bitmask |
| `--hours` | hex | Hour-slot access bitmask (bits 0-23) |
| `--baud-hours` | hex | Baud rate hour restriction bitmask |

**Gender restriction:**

| Option | Type | Description |
|--------|------|-------------|
| `--gender` | string | Gender restriction: `any` (or `0`, `none`), `M` (or `m`), `F` (or `f`) |

**Boolean flags** (all accept `true` or `false`):

| Option | Field | Description |
|--------|-------|-------------|
| `--closed` | Closed | Close subboard to new posts |
| `--real-names` | RealNames | Require real names |
| `--anonymous` | Anonymous | Allow anonymous posting |
| `--private` | PrivateArea | Private area |
| `--no-mci` | NoMCI | Disable MCI processing |
| `--verification` | Verification | Require file verification |
| `--dup-check` | DupCheck | Enable duplicate checking |
| `--show-unvalidated` | ShowUnvalidated | Show unvalidated items |
| `--no-signatures` | NoSignatures | Disable signatures |
| `--no-read-charges` | NoReadCharges | Disable read charges |
| `--no-write-charges` | NoWriteCharges | Disable write charges |
| `--invitation` | Invitation | Invitation-only access |
| `--user-must-join` | UserMustJoin | Users must join before accessing |
| `--delete-own` | DeleteOwn | Allow users to delete own posts |
| `--carbon-copy` | CarbonCopy | Enable carbon copy |
| `--cdrom` | CDROM | CD-ROM mode |
| `--qwk-replies` | QWKReplies | Allow QWK replies |
| `--persist` | Persist | Keep data in memory |
| `--delay` | Delay | Delay processing |
| `--diz-save` | DizSave | Save FILE_ID.DIZ |

**Obits (option bit) flags** (all accept `true` or `false`):

| Option | Bit | Description |
|--------|-----|-------------|
| `--obit-showbows` | OFF_SHOWBOWS | Show bows |
| `--obit-diz-alnum` | OFF_DIZALNUM | DIZ alphanumeric filter |
| `--obit-diz-strip-chars` | OFF_DIZSTRIPCHARS | DIZ strip special characters |
| `--obit-diz-strip-text` | OFF_DIZSTRIPTEXT | DIZ strip text |
| `--obit-diz-strip-cr` | OFF_DIZSTRIPCR | DIZ strip carriage returns |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"updated"` |

Plus all summary fields and all detail fields (same as `sub show` output).

### Notes
- Boolean flags use `true`/`false` strings. Any value that is not exactly `"true"` is treated as false.
- Obit flags use bitwise OR/AND operations: `--obit-showbows true` sets the bit, `false` clears it.
- Access fields that accept group strings (e.g., `--access "1-3,5"`) use `ConvertAccess()` from cnet.library. Hex fields that do NOT accept group strings (e.g., `--hours`, `--computer-types`) use plain hex parsing.

---

## `cnet-cli sub delete`

### Synopsis
```
cnet-cli sub delete <sub-id|gokey> [--force]
```

### Description
Marks a subboard as killed (sets `MRK_SUBBOARD_KILLED` bit in the Marker field). Unlinks the subboard from its parent's child chain. Does NOT delete the data directory or its files.

If the subboard has children and `--force` is not specified, the command fails. With `--force`, all children are reparented to the deleted subboard's parent before deletion.

This is a write operation. Acquires SEM[5] exclusive.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Subboard identifier |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--force` | flag | off | Reparent children to the deleted subboard's parent and proceed with deletion |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"deleted"` |
| `physnum` | number | Physical number of the deleted subboard |
| `title` | string | Title of the deleted subboard (MCI stripped) |

### Notes
- Cannot delete the root subboard.
- Cannot delete a subboard that is already killed.
- With `--force`, the children are spliced into the parent's child chain at the position the deleted node occupied. All reparented children and affected siblings are written to disk.
- If the subboard has no valid parent (orphaned), it is simply marked as killed with Child and Next set to -1. No tree fixup is performed.
- Dual writes: the deleted subboard, its parent, and all affected siblings/children are written to disk.
