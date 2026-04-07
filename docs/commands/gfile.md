# GFile Commands

GFile (General Text File) operations for CNet BBS. GFile areas use the same MRK_TEXT_DOOR (3) subboard marker and identical data format as news areas. All GFile commands are thin wrappers that validate arguments with GFile-specific error messages and then delegate to the corresponding news handler.

All GFile commands require `cnet.library` to be open and CNet to be running.

## Relationship to News Commands

GFile commands map directly to news commands:

| GFile Command | Delegates To | Description |
|---------------|-------------|-------------|
| `gfile list` | `news list` | List items |
| `gfile read` | `news read` | Read item content |
| `gfile add` | `news post` | Add new item |
| `gfile remove` | `news delete` | Delete item |

The output format, behavior, and semantics are identical. The GFile wrappers exist to provide a separate namespace and GFile-specific usage messages. There is no `gfile edit` command; use `news edit` to modify GFile items.

---

## `cnet-cli gfile list`

### Synopsis
```
cnet-cli gfile list <sub-id|gokey> [--limit N] [--offset N]
```

### Description
Lists items in a GFile (general text file) subboard. Delegates to `news list`.

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

Identical to `news list`. See [News Commands](news.md#cnet-cli-news-list) for full field documentation.

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `subboard` | string | GO key of the subboard |
| `physnum` | number | Physical subboard number |
| `items` | array | Array of item objects |
| `total` | number | Total item count |

Each item object contains: `number`, `index`, `title`, `by_account`, `by_handle`, `post_date`, `killed`, `frozen`.

### Notes
- The subboard must be a text/door area (MRK_TEXT_DOOR). The type validation occurs inside the delegated `news list` handler.

---

## `cnet-cli gfile read`

### Synopsis
```
cnet-cli gfile read <sub-id|gokey> <item-number>
```

### Description
Reads the full content of a GFile item. Delegates to `news read`.

This is a read-only operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index within the subboard |

### Output Fields

Identical to `news read`. See [News Commands](news.md#cnet-cli-news-read) for full field documentation.

Key fields: `item.number`, `item.index`, `item.title`, `item.by_account`, `item.by_handle`, `item.post_date`, `item.text`, `item.text_format`, `item.killed`, `item.frozen`, `item.auto_grab`.

### Notes
- Item number is 1-based.
- Text format detection (header/plain/file/none) is handled by the news read handler. See the [News Commands](news.md#text-format-values) documentation for details on text storage formats.

---

## `cnet-cli gfile add`

### Synopsis
```
cnet-cli gfile add <sub-id|gokey> --title <title> --author <account> --text <text>
```

### Description
Adds a new text item to a GFile subboard. Delegates to `news post`. Creates a physical text file in the subboard's DataPath directory and records it in the subboard data.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--title` | string | (required) | Title of the GFile item |
| `--author` | integer | (required) | Account number of the author |
| `--text` | string | (required) | Text content of the item |

### Output Fields

Identical to `news post`. See [News Commands](news.md#cnet-cli-news-post) for full field documentation.

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"posted"` on success |
| `physnum` | number | Physical subboard number |
| `item_count` | number | New total item count |
| `next_id` | number | Next available unique ID |
| `title` | string | Item title |
| `by_account` | number | Author account number |
| `text_offset` | number | Offset in `_text` for the path string |
| `text_file` | string | Path to the created text file |

### Notes
- All three options (`--title`, `--author`, `--text`) are required.
- `--author` must be a valid, existing account number.
- The physical text file is created at `<DataPath>/item_<id>.txt`.
- See [News Commands](news.md#cnet-cli-news-post) for full behavioral details.

---

## `cnet-cli gfile remove`

### Synopsis
```
cnet-cli gfile remove <sub-id|gokey> <item-number>
```

### Description
Marks a GFile item as killed. Delegates to `news delete`. The item remains in the subboard data but is logically deleted.

This is a write operation.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<sub-id\|gokey>` | Yes | Physical subboard number or GO key |
| `<item-number>` | Yes | 1-based item index |

### Output Fields

Identical to `news delete`. See [News Commands](news.md#cnet-cli-news-delete) for full field documentation.

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"deleted"` on success |
| `physnum` | number | Physical subboard number |
| `item_index` | number | 1-based item index |
| `item_number` | number | Unique item ID |
| `title` | string | Item title |

### Notes
- Item number is 1-based.
- There is no `--delete-physical` option (unlike `file remove`). The physical text file is not deleted.
- The `_text` pool space is not freed. Use `maint repair-sub` to compact the text pool if needed.
