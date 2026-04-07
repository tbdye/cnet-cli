# News Commands

News operations for CNet BBS text/door subboards (MRK_TEXT_DOOR). News areas store text items with optional HeaderType metadata. Each item's text is stored either as a physical DOS file (referenced by path in `_text`) or as an inline HeaderType record in `_text`.

All news commands require `cnet.library` to be open and CNet to be running. Subboard data files are loaded via `OneMoreUser`/`OneLessUser` internally.

## `cnet-cli news list`

### Synopsis
```
cnet-cli news list <sub-id|gokey> [--limit N] [--offset N]
```

### Description
Lists items in a news/text-door subboard. Returns basic metadata for each item. Items are returned in subboard order (0-based internal index, exposed as 1-based `index`).

This is a read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number (0-based) or GO key (SubDirName, case-insensitive) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--limit` | number | 0 (no limit) | Maximum number of items to return |
| `--offset` | number | 0 | Number of items to skip from the beginning |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key (SubDirName) of the subboard, MCI codes stripped |
| `physnum` | number | Physical subboard number |
| `items` | array | Array of news item objects |
| `total` | number | Total number of items in the subboard (regardless of limit/offset) |

Each object in `items`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item ID (ihead.Number) |
| `index` | number | 1-based display index within the subboard |
| `title` | string | Item title (MCI stripped) |
| `by_account` | number | Account number of the author |
| `by_handle` | string | Handle of the author (MCI stripped) |
| `post_date` | string\|null | Post date in ISO 8601 format (YYYY-MM-DDTHH:MM:SS), or null if unset |
| `killed` | boolean | Whether the item is marked as killed/deleted |
| `frozen` | boolean | Whether the item is frozen |

### Notes
- The subboard must be a text/door area (MRK_TEXT_DOOR). Returns an error if the subboard is a different type.
- Killed items are included in the listing. Filter on `killed` if you want only live items.
- `total` reflects the full item count, not the number returned after limit/offset filtering.
- News list output has fewer fields than file list (no size, downloads, validated, etc.) because news items are text, not files.

---

## `cnet-cli news read`

### Synopsis
```
cnet-cli news read <sub-id|gokey> <item-number>
```

### Description
Reads the full content of a news item, including its text body. Handles three text storage formats: HeaderType (old inline format), plain text (legacy), and DOS file path (new format where `_text` stores a path to a physical text file).

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
| `item` | object | The news item detail object |

Fields within `item`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item ID |
| `index` | number | 1-based display index |
| `title` | string | Item title (MCI stripped) |
| `by_account` | number | Author's account number |
| `by_handle` | string | Author's handle |
| `post_date` | string\|null | Post date (ISO 8601) or null |

**HeaderType metadata (present only for "header" text_format):**

| Field | Type | Description |
|-------|------|-------------|
| `by_name` | string | Author's real name from HeaderType (only if non-empty) |
| `by_user` | string | Author's handle from HeaderType (only if non-empty) |
| `post_date_header` | string | Post date from HeaderType record (ISO 8601, only if non-null) |
| `org` | string | Organization field from HeaderType (only if non-empty) |

**Status flags:**

| Field | Type | Description |
|-------|------|-------------|
| `killed` | boolean | Whether the item is killed |
| `frozen` | boolean | Whether the item is frozen |
| `auto_grab` | boolean | Auto-grab flag |

**Text content:**

| Field | Type | Description |
|-------|------|-------------|
| `text` | string\|null | The item text content, or null if unavailable |
| `text_error` | string | Error message if text could not be read (only present on error) |
| `text_file` | string | Physical file path (only present for "file" format when the raw path string was read) |
| `text_format` | string | Format of the text: `"header"`, `"plain"`, `"file"`, or `"none"` |

### Text Format Values

| Format | Description |
|--------|-------------|
| `header` | Text stored in a HeaderType record within `_text`. The HeaderType contains metadata (author, dates, org) and a pointer to the body text. |
| `plain` | Legacy plain text stored directly in `_text` at the item's seek offset. |
| `file` | Text stored in a separate DOS file. `_text` contains the file path string. The text is read from that path. |
| `none` | No text content (item.First < 0, or text could not be read). |

### Notes
- Item number is 1-based.
- The command auto-detects the text storage format by checking for the HeaderType magic value (0xBB25B8C4) at the item's text offset.
- For "file" format, the path stored in `_text` is an AmigaOS path (e.g., `SYS:cnet/gfiles/item_42.txt`).
- If the text file cannot be read in "file" format, `text` will be null and `text_error` will contain the error message.

---

## `cnet-cli news post`

### Synopsis
```
cnet-cli news post <sub-id|gokey> --title <title> --author <account> --text <text>
```

### Description
Creates a new news item in a text/door subboard. The text content is written to a physical DOS file in the subboard's DataPath directory, and the file path is stored in `_text` via AllocText. The item is automatically marked as validated and finished.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--title` | string | (required) | Title of the news item |
| `--author` | number | (required) | Account number of the author |
| `--text` | string | (required) | Text content of the news item |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"posted"` on success |
| `physnum` | number | Physical subboard number |
| `item_count` | number | New total item count in the subboard |
| `next_id` | number | Next available unique ID (sub->count) |
| `title` | string | Item title as stored |
| `by_account` | number | Author account number |
| `text_offset` | number | Offset in `_text` where the path string was written |
| `text_file` | string | Full AmigaOS path to the physical text file created |

### Notes
- `--author` must be an existing account number. The author's ID is looked up from the Key array to verify existence.
- The physical text file is created at `<DataPath>/item_<id>.txt` where `<id>` is the assigned unique item ID.
- If the text file creation fails or `AllocText` fails, the command cleans up (deletes the created file if necessary, frees allocated text pool space) and returns an error.
- Items are created with: `Validated=1`, `Finished=1`, `AutoGrab=1`, `PurgeKill=1`. `PurgeStatus` and `DLnotifyULer` are inherited from the subboard configuration.
- `ihead.Size` is set to 0 (news items have no file size).
- Requires exclusive semaphore on the subboard during the post operation.
- The subboard is persisted to disk after the add operation.

---

## `cnet-cli news edit`

### Synopsis
```
cnet-cli news edit <sub-id|gokey> <item-number> [--text <text>] [--title <title>]
```

### Description
Edits an existing news item. Can update the title, the text content, or both. The text update behavior depends on the storage format: for DOS file format items, the physical file is overwritten; for HeaderType format items, the old text is freed and new text is allocated.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--text` | string | (unchanged) | New text content for the item |
| `--title` | string | (unchanged) | New title for the item |

At least one of `--text` or `--title` must be specified.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"edited"` on success |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `old_format` | string | Original text format: `"header"` or `"file"` (only present if `--text` was used) |
| `title` | string | New title (only present if `--title` was used) |
| `header_offset` | number | New HeaderType offset in `_text` (only present for "header" format edits with `--text`) |
| `body_offset` | number | New body text offset in `_text` (only present for "header" format edits with `--text`) |
| `text_file` | string | Path to the overwritten text file (only present for "file" format edits with `--text`) |

### Notes
- Item number is 1-based.
- The item must have existing text content (`item.First >= 0`) if `--text` is specified. Items with no text content cannot have text added via edit.
- **File format items**: The physical DOS file is overwritten in place with the new content (opened with MODE_NEWFILE, which truncates).
- **HeaderType format items**: The old body text and HeaderType record are freed via `FreeText`, then new allocations are made via dual `AllocText`. The HeaderType's `EditDate` is updated to the current time. If the old item was plain text (no HeaderType), it is upgraded to HeaderType format.
- `--title` updates the item's Title field and the TitleSort (uppercase first 8 characters).
- The subboard is persisted to disk after the edit operation.

---

## `cnet-cli news delete`

### Synopsis
```
cnet-cli news delete <sub-id|gokey> <item-number>
```

### Description
Marks a news item as killed by setting `ihead.Killed = 1`. The item remains in the subboard data but is logically deleted.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"deleted"` on success |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Item title (MCI stripped) |

### Notes
- Item number is 1-based.
- Unlike `file remove`, there is no check for whether the item is already killed. Re-killing an already-killed item is a no-op that succeeds.
- There is no option to delete the physical text file. The `_text` pool space is not freed. Use `maint repair-sub` to compact the text pool.
- The item data is written back via `ZPutItem` with the updated Killed flag.
