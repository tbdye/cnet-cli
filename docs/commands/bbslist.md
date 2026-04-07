# BBS List Commands

BBS List commands read the CNet BBS directory -- a list of other BBSes added by users. The data is stored as sequential 158-byte `BBSItem` records in the file `sysdata:bbslist/bbslist`.

---

## `cnet-cli bbslist list`

### Synopsis
```
cnet-cli bbslist list [--all]
```

### Description
Lists all entries in the BBS directory. By default, killed (deleted) entries are excluded from the output. Use `--all` to include them.

This is a read-only command. No semaphore is acquired (there is no dedicated BBSList semaphore in MainPort).

### Arguments
None (positional).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--all` | flag | off | Include killed (deleted) entries in the listing. When enabled, each entry gains a `killed` boolean field |

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `entries` | array | Array of BBS directory entry objects |
| `count` | number | Number of entries emitted (after filtering) |
| `total_records` | number | Total number of records in the file (including killed entries) |
| `warnings` | array | Present only if there are warnings (e.g., file size alignment issues) |

Each element of `entries`:

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | 0-based record index in the file |
| `user_id` | number | ID number of the user who added this entry |
| `phone` | string | Phone number of the BBS |
| `title` | string | Name/title of the BBS |
| `location` | string | City/state or geographic location |
| `baud` | string | Baud rate string |
| `comments` | string | Description or comments about the BBS |
| `country` | string | Country code |
| `flags` | string | Feature flags string |
| `immortal` | boolean | Whether the entry is protected from automatic purging |
| `date` | string or null | Date the entry was added (formatted string), or `null` if no date is set |
| `killed` | boolean | Whether the entry is deleted. **Only present when `--all` is used** |

### Notes
- If the BBSList file does not exist, an empty result is returned (`entries: [], count: 0, total_records: 0`) with no error.
- A warning is emitted if the file size is not an exact multiple of the 158-byte record size, which may indicate B-tree index data appended to the file.
- The `date` field uses the CNet `IsDate` format (6-byte year/month/day/hour/minute/second). Null dates (all zeros) are emitted as `null`.
- The `index` field is the physical position in the file (0-based), which includes killed entries in the numbering even when `--all` is not used. This means index values may have gaps in the default (non-`--all`) output.
