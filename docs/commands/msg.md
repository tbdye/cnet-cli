# Message Commands

Message commands operate on items (posts) and responses within CNet BBS message base subboards. All `msg` commands require a target subboard that is of type MsgBase (marker 0); they reject FileTxfer, door, and subdirectory types.

## Subboard Resolution

All commands that accept a `<sub-id|gokey>` argument use the same `resolve_subboard()` function described in the subboard documentation. It accepts a physical number, a GO key (case-insensitive), or a unique name via `NumFromUnique()`.

## Item Numbering

Messages use two numbering schemes:

- **Item index** (`item_index`): A 1-based position within the subboard's item array. This is what you pass on the command line (the `<item-number>` argument).
- **Item number** (`item_number` / `ihead.Number`): A unique ID assigned from the subboard's `count` sequence. This value never changes and is used internally for cross-references between items and responses.

## Data Model

Each message base subboard has these data files in its `data/` directory:

- **_Items3**: Array of `ItemType3` records (168 bytes each) -- metadata per item.
- **_Headers3**: Array of `ItemHeader` records (34 bytes each) -- item header with number, dates, response count.
- **_Message3**: Array of `MessageType3` records (28 bytes each) -- response metadata.
- **_text**: Variable-length text pool. Contains `HeaderType` records (288 bytes, magic `0xBB25B8C4`) and null-terminated body text. Items and responses point into this file.
- **_Free**: Free-list for the text pool, managed by `AllocText`/`FreeText`/`SaveFree`.

## OneMoreUser / OneLessUser Lifecycle

All `msg` commands that access item/header data call `OneMoreUser(sub, 0)` to load the data files into memory, and `OneLessUser(sub)` on cleanup. `OneMoreUser` does NOT load `_Message3` -- responses are read directly from disk via `load_messages()` when needed.

---

## `cnet-cli msg list`

### Synopsis
```
cnet-cli msg list <sub-id|gokey> [--limit N] [--offset N]
```

### Description
Lists items (message posts) in a message base subboard. Returns summary information for each item including title, author, response count, and post date. Supports pagination via `--limit` and `--offset`.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--limit` | number | 0 (no limit) | Maximum number of items to return |
| `--offset` | number | 0 | Number of items to skip from the beginning (0-based) |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key of the subboard (MCI stripped) |
| `physnum` | number | Physical subboard number |
| `items` | array | Array of item summary objects |
| `total` | number | Total item count in the subboard (before limit/offset) |

Each element of `items`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item number (from ItemHeader.Number) |
| `index` | number | 1-based item index within the subboard |
| `title` | string | Item title (MCI stripped) |
| `by_account` | number | Author's account number |
| `by_handle` | string | Author's handle (MCI stripped, looked up from Key array) |
| `responses` | number | Number of responses to this item |
| `post_date` | string or null | Post date as ISO 8601 string (`"YYYY-MM-DDTHH:MM:SS"`), or null if unset |
| `killed` | boolean | Whether the item is marked as deleted |
| `size` | number | Item size (0 for message posts, file size for file entries) |

### Notes
- Items are returned in array order (oldest first by default).
- Killed items are included in the listing; filter on the `killed` field if needed.
- The `index` field is 1-based (the value you pass to `msg read`, `msg delete`, etc.).

---

## `cnet-cli msg read`

### Synopsis
```
cnet-cli msg read <sub-id|gokey> <item-number>
```

### Description
Reads a single message item with its full text content and all responses. The item number is 1-based (the `index` field from `msg list`).

The command attempts to read the text via the HeaderType format (magic `0xBB25B8C4`). If the magic does not match, it falls back to reading raw text at the stored offset (legacy format).

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |
| `<item-number>` | Yes | 1-based item index |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `item` | object | The message item with all details |

Fields within `item`:

**Core fields (always present):**

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique item number |
| `index` | number | 1-based item index (same as the argument) |
| `title` | string | Item title (MCI stripped) |
| `by_account` | number | Author's account number |
| `by_handle` | string | Author's handle (MCI stripped) |
| `post_date` | string or null | Post date as ISO 8601, or null |
| `responses` | number | Number of responses |
| `killed` | boolean | Whether the item is killed |
| `size` | number | Item size |
| `text` | string or null | Message body text, or null if unavailable |

**Recipient fields (present only if `item.ToID != 0`):**

| Field | Type | Description |
|-------|------|-------------|
| `to_handle` | string | Recipient's handle (if account resolved) |
| `to_id` | number | Recipient's ID number (if account could not be resolved) |

**HeaderType metadata fields (present only for HeaderType-format messages):**

| Field | Type | Description |
|-------|------|-------------|
| `by_name` | string | Author's real name from HeaderType.By (if non-empty) |
| `by_user` | string | Author's handle from HeaderType.ByUser (if non-empty) |
| `to_name` | string | Recipient's real name from HeaderType.To (if non-empty, and ToID != 0) |
| `to_user` | string | Recipient's handle from HeaderType.ToUser (if non-empty, and ToID != 0) |
| `post_date_header` | string | Post date from the HeaderType record (may differ from ihead.PostDate) |
| `org` | string | Organization field from HeaderType.Organ (if non-empty) |

**Responses:**

| Field | Type | Description |
|-------|------|-------------|
| `responses_list` | array | Array of response objects |

Each element of `responses_list`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | Unique response number (from MessageType3.Number) |
| `by_handle` | string or null | Response author's handle (resolved from ByID via IDToAccount), or null |
| `by_id` | number | Response author's ID (present instead of by_handle if account could not be resolved) |
| `text` | string or null | Response body text, or null |
| `post_date` | string or null | Response post date as ISO 8601, or null |

**Response HeaderType metadata (present per-response if the response uses HeaderType format):**

| Field | Type | Description |
|-------|------|-------------|
| `by_name` | string | Author's real name (if non-empty) |
| `by_user` | string | Author's handle from HeaderType (if non-empty) |
| `to_name` | string | Recipient's real name (if non-empty, and ToID != 0) |
| `to_user` | string | Recipient's handle from HeaderType (if non-empty, and ToID != 0) |
| `org` | string | Organization field (if non-empty) |

### Notes
- Responses are loaded from `_Message3` on disk (not from in-memory data). They are matched by `msg.ItemNumber == ihead.Number`.
- Reading stops early once the expected number of responses (from `ihead.Responses`) have been found.
- Legacy messages (pre-HeaderType format) are still readable but lack the extended metadata fields (`by_name`, `org`, etc.).

---

## `cnet-cli msg post`

### Synopsis
```
cnet-cli msg post <sub-id|gokey> --title <title> --author <account>
    --text <text> | --file <path> [--to <account>]
```

### Description
Posts a new message item to a message base subboard. Creates a HeaderType record and body text in the `_text` file, an ItemType3 record in `_Items3`, and an ItemHeader in `_Headers3`. Increments the subboard's unique ID counter.

This is a write operation. Acquires sub->sem for text pool operations and SEM[5] for disk persistence.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--title` | string | (required) | Message title |
| `--author` | number | (required) | Author's account number |
| `--text` | string | (one required) | Message body text (inline) |
| `--file` | string | (one required) | AmigaOS path to a text file containing the message body (max 65536 bytes). Mutually exclusive with `--text`. |
| `--to` | number | (none) | Recipient's account number (for directed messages) |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"posted"` |
| `physnum` | number | Physical subboard number |
| `item_count` | number | Updated item count in the subboard |
| `next_id` | number | Updated next unique ID counter |
| `title` | string | The posted message title |
| `by_account` | number | Author's account number |
| `header_offset` | number | Byte offset of the HeaderType in _text |
| `body_offset` | number | Byte offset of the body text in _text |

### Notes
- The author account must exist and have a valid IDNumber (non-zero).
- Author's real name and handle are automatically looked up from the Key array (SEM[1] shared).
- The item is created with `Validated=1`, `Finished=1`, `PurgeKill=1`.
- Cannot use both `--text` and `--file` simultaneously.
- `--file` reads from an AmigaOS filesystem path on the Amiga side, not a host path.

---

## `cnet-cli msg respond`

### Synopsis
```
cnet-cli msg respond <sub-id|gokey> <item-number> --author <account>
    --text <text> | --file <path> [--to <account>]
```

### Description
Adds a response to an existing message item. Creates a new HeaderType + body text in `_text`, a MessageType3 record in `_Message3`, updates the item's Last pointer and response count, and patches the previous last HeaderType's Next pointer to form a linked list.

This is a write operation. Acquires sub->sem for text pool and linked list operations.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |
| `<item-number>` | Yes | 1-based item index to respond to |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--author` | number | (required) | Response author's account number |
| `--text` | string | (one required) | Response body text (inline) |
| `--file` | string | (one required) | AmigaOS path to a text file containing the response body. Mutually exclusive with `--text`. |
| `--to` | number | (none) | Recipient's account number |

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"responded"` |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index that was responded to |
| `item_number` | number | Unique number of the parent item |
| `response_number` | number | Unique number assigned to the new response |
| `responses` | number | Updated response count for the item |
| `by_account` | number | Response author's account number |
| `header_offset` | number | Byte offset of the new HeaderType in _text |
| `body_offset` | number | Byte offset of the new body text in _text |
| `warnings` | array | Optional array of warning strings (e.g., linked-list patch failures) |

### Notes
- The linked list is maintained by patching the previous last response's `HeaderType.Next` to point to the new response. If this patch write fails, a warning is emitted but the response is still created.
- The response's `HeaderType.Previous` points to the previous last entry (item.Last before update).
- `ihead.Responses` is incremented, `ihead.RespDate` is set to current time, and `sub->nNewMess` is incremented.

---

## `cnet-cli msg delete`

### Synopsis
```
cnet-cli msg delete <sub-id|gokey> <item-number>
```

### Description
Marks a message item as killed by setting `ihead.Killed = 1`. The item is not physically removed from the data files; it remains in the array and can still appear in listings with `killed: true`.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |
| `<item-number>` | Yes | 1-based item index to delete |

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"deleted"` |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index that was deleted |
| `item_number` | number | Unique number of the deleted item |
| `title` | string | Title of the deleted item (MCI stripped) |

### Notes
- This is a soft delete (sets a killed flag). The item data and text remain on disk.
- Does not delete or modify responses to the item.
- Does not update the subboard's item count (`sub->rn`).

---

## `cnet-cli msg edit`

### Synopsis
```
cnet-cli msg edit <sub-id|gokey> <item-number>
    [--text <text> | --file <path>] [--title <title>] [--response N]
```

### Description
Edits the text and/or title of an existing message item or one of its responses. For text edits, the old HeaderType and body are freed from the text pool, a new HeaderType + body are written, and all linked list pointers are patched.

This is a write operation. Acquires sub->sem for text pool operations.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Target subboard identifier |
| `<item-number>` | Yes | 1-based item index to edit |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--text` | string | (none) | New message body text (inline) |
| `--file` | string | (none) | AmigaOS path to a text file with the new body. Mutually exclusive with `--text`. |
| `--title` | string | (none) | New item title. Ignored when `--response` is specified. |
| `--response` | number | (none) | Edit the N-th response (1-based) instead of the original post. Requires `--text` or `--file`. |

At least one of `--text`, `--file`, or `--title` must be specified (unless `--response` is used, in which case `--text` or `--file` is required).

### Output Fields

**Item post edit (no `--response`):**

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"edited"` |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item number |
| `header_offset` | number | New HeaderType byte offset in _text (present only if text was changed) |
| `body_offset` | number | New body text byte offset in _text (present only if text was changed) |
| `edit_date` | string | Edit timestamp as ISO 8601 (present only if text was changed) |
| `title_changed` | boolean | Whether the title was modified |
| `text_changed` | boolean | Whether the body text was modified |

**Response edit (`--response N`):**

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"edited"` |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item number |
| `response` | number | Which response was edited (the N from `--response N`) |
| `response_number` | number | Unique number of the edited response |
| `header_offset` | number | New HeaderType byte offset |
| `body_offset` | number | New body text byte offset |
| `edit_date` | string | Edit timestamp as ISO 8601 |
| `text_changed` | boolean | Always true for response edits |

### Notes
- Only HeaderType-format messages can be text-edited. Legacy-format messages (no HeaderType magic) return an error.
- The `EditDate` field in the HeaderType is updated to the current time on text edits.
- All other HeaderType fields (By, To, PostDate, etc.) are preserved from the original.
- For response edits, the N-th response is found by scanning `_Message3` records matching `ihead.Number` and counting matches sequentially.
- Linked list integrity is maintained: Previous/Next pointers in neighboring HeaderType records are patched to point to the new position.
- If `item.First` or `item.Last` pointed to the old position, they are updated.

---

## `cnet-cli msg search`

### Synopsis
```
cnet-cli msg search <query> [--sub <sub-id|gokey>] [--limit N]
    [--field title|text|by] [--from <account>]
```

### Description
Searches for messages across one or all message base subboards. By default searches item titles; can also search body text or author handles. Returns matching items with their metadata.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<query>` | Yes | Search string (case-insensitive substring match) |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--sub` | string | (all MsgBase subs) | Restrict search to a specific subboard. If omitted, searches all non-killed, non-subdirectory MsgBase subboards. |
| `--limit` | number | 100 | Maximum number of matches to return |
| `--field` | string | `title` | Which field to search: `title` (item title), `text` (body text), or `by` (author handle) |
| `--from` | number | (none) | Filter by author account number. When combined with `--field by`, the query supplements the filter; with other fields, both must match. |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `query` | string | The search query string |
| `field` | string | The field that was searched (`"title"`, `"text"`, or `"by"`) |
| `limit` | number | The effective limit |
| `matches` | array | Array of matching item objects |
| `skipped` | array | Array of physical subboard numbers that could not be loaded (OneMoreUser failed) |
| `subboards_searched` | number | Number of subboards successfully searched |
| `total_matches` | number | Total number of matches found |

Each element of `matches`:

| Field | Type | Description |
|-------|------|-------------|
| `physnum` | number | Physical subboard number |
| `subboard` | string | GO key of the subboard (MCI stripped) |
| `item_index` | number | 1-based item index within the subboard |
| `item_number` | number | Unique item number |
| `title` | string | Item title (MCI stripped) |
| `by_account` | number | Author's account number |
| `by_handle` | string | Author's handle (MCI stripped) |
| `post_date` | string or null | Post date as ISO 8601, or null |
| `responses` | number | Number of responses |

### Notes
- Killed items are excluded from search results.
- When searching all subboards (no `--sub`), subdirectory nodes are skipped.
- Maximum 256 subboards are searched in a single query.
- `--field text` requires reading the body text from `_text` for each item, which is significantly slower than title search.
- The `--from` filter is applied before the query match, making it an efficient pre-filter for author-specific searches.
- Body text search reads via the HeaderType-aware reader and supports both legacy and new format messages.

---

## `cnet-cli msg move`

### Synopsis
```
cnet-cli msg move <src-sub> <item-number> <dst-sub>
```

### Description
Moves a message item (with all its responses) from one message base subboard to another. The operation has three phases:

1. **Read**: Load the source item, its HeaderType + body text, and all responses from the source subboard.
2. **Write**: Create a new item in the destination subboard with new unique IDs, write all text to the destination's `_text` file, and build a new response chain.
3. **Delete**: Mark the source item as killed (soft delete).

If phase 2 fails partway through (e.g., text pool full), the source item is preserved and the destination may have a partial copy. The status field indicates `"partial_move"` in this case.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<src-sub>` | Yes | Source subboard identifier |
| `<item-number>` | Yes | 1-based item index in the source subboard |
| `<dst-sub>` | Yes | Destination subboard identifier |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"moved"` on success, `"partial_move"` if response copy failed |
| `warning` | string | Present only on partial move; describes what happened |

Source information:

| Field | Type | Description |
|-------|------|-------------|
| `source` | object | Source item details |
| `source.physnum` | number | Source subboard physical number |
| `source.subboard` | string | Source subboard GO key |
| `source.item_index` | number | 1-based item index in source |
| `source.item_number` | number | Original unique item number |
| `source.responses` | number | Number of responses read from source |
| `source.killed` | boolean | Present only on partial move (false, since source is preserved) |

Destination information:

| Field | Type | Description |
|-------|------|-------------|
| `destination` | object | Destination item details |
| `destination.physnum` | number | Destination subboard physical number |
| `destination.subboard` | string | Destination subboard GO key |
| `destination.item_index` | number | 1-based item index in destination |
| `destination.new_item_number` | number | New unique item number in destination |
| `destination.responses_copied` | number | Number of responses successfully copied |
| `destination.header_offset` | number | Byte offset of the new item's HeaderType in destination _text |

**Response count mismatch warning (conditional):**

| Field | Type | Description |
|-------|------|-------------|
| `response_count_warning` | string | Present if `_Message3` response count differs from `ihead.Responses` |
| `ihead_responses` | number | Expected response count from the item header |
| `actual_responses` | number | Actual number of responses found in `_Message3` |

### Notes
- Both source and destination must be MsgBase subboards.
- Source and destination cannot be the same subboard.
- Killed source items cannot be moved.
- Only HeaderType-format messages can be moved. Legacy-format messages return an error.
- New unique IDs are assigned from the destination subboard's counter for both the item and all responses.
- The response linked list (HeaderType.Previous/Next) is rebuilt in the destination.
- All original metadata (author info, dates, organization, etc.) is preserved from the source HeaderType via struct copy.
- For items with `ihead.Number == 0` (legacy), responses are read by walking the HeaderType linked list instead of scanning `_Message3` by ItemNumber. This avoids ambiguity when multiple items share Number 0.
- Maximum 10000 responses can be walked in linked-list mode (prevents infinite loops from corrupted chains).
- On a successful full move, the source item is marked killed. On partial failure, the source is preserved.
