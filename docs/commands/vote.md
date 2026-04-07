# Vote Commands

Vote commands provide read-only access to the CNet BBS global voting booth. Vote topics are stored in `SysData:Vote/topics` as 94-byte records. Each topic's choices, vote tallies, and descriptive text are stored in per-topic subdirectories under `SysData:Vote/{topic-number}/`.

All vote commands acquire `SEM[14]` (the voting booth semaphore) in shared mode for the duration of the read.

---

## `cnet-cli vote list`

### Synopsis
```
cnet-cli vote list
```

### Description
Lists all vote topics in the system. Returns summary information for each topic including its name, serial number, accessibility flag, and maximum choices allowed.

This is a read-only command.

### Arguments
None.

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `topics` | array | Array of vote topic objects |
| `count` | number | Number of topics emitted |
| `warnings` | array | Present only if there are warnings (e.g., file size alignment issues) |

Each element of `topics`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | 1-based topic number |
| `name` | string | Topic name (MCI codes stripped) |
| `serial` | number | Unique serial number for this topic |
| `float` | boolean | Global accessibility flag (whether the topic floats to all areas) |
| `max_choices` | number | Maximum number of choices a user can select |
| `field_62_hex` | string | Unknown field at offset 62 as 8-character hex (possibly creation date) |
| `field_66_hex` | string | Unknown field at offset 66 as 12-character hex (6 bytes, possibly IsDate) |
| `field_76_hex` | string | Unknown field at offset 76 as 8-character hex |
| `field_80_hex` | string | Unknown field at offset 80 as 8-character hex |
| `field_84` | number | Unknown field at offset 84 (possibly current choice count) |
| `field_86_hex` | string | Unknown field at offset 86 as 8-character hex (probable creator user ID) |
| `field_91` | number | Unknown field at offset 91 (possibly killed flag) |

### Notes
- If the topics file does not exist, an empty result is returned (`topics: [], count: 0`) with no error.
- A warning is emitted if the file size is not an exact multiple of the 94-byte record size.
- Topic numbers are 1-based (the first topic in the file is topic 1).
- Fields with unknown semantics are emitted as hex strings or raw integers to preserve the data for inspection.
- Acquires SEM[14] shared.

---

## `cnet-cli vote show`

### Synopsis
```
cnet-cli vote show <topic-number>
```

### Description
Displays full detail for a single vote topic, including all topic fields, the list of choices with their text, per-choice vote totals, voter count, and the topic's descriptive text.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<topic-number>` | Yes | 1-based topic number. Must be all digits and >= 1 |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `topic` | object | The vote topic detail object |
| `warnings` | array | Present only if there are warnings |

The `topic` object contains all fields from `vote list` (see above), plus:

| Field | Type | Description |
|-------|------|-------------|
| `choices` | array | Array of choice objects |
| `choice_count` | number | Number of choices for this topic |
| `results` | array | Array of vote counts (integers), one per choice, in the same order as `choices` |
| `total_votes` | number | Sum of all vote counts across all choices |
| `voter_count` | number | Number of distinct users who have voted on this topic |
| `text` | string or null | Descriptive text for the topic, or `null` if no text file exists or the file is empty |

Each element of `choices`:

| Field | Type | Description |
|-------|------|-------------|
| `number` | number | 1-based choice number |
| `text` | string | Choice text (MCI codes stripped) |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"No vote topics configured"` -- topics file does not exist
- `"Vote topic N out of range (1-M)"` -- topic number exceeds the number of topics in the file
- `"Topic number must be >= 1"` -- topic number is zero or negative
- `"Failed to read vote topic"` -- read error on the topics file

### Notes
- The `results` array contains raw integer vote counts, one per choice, in order. The array position corresponds to the choice number (index 0 = choice 1).
- The `voter_count` is derived from the talley file (`SysData:Vote/{N}/talley`), which stores one 104-byte record per voter containing the user's ID and a 100-byte vote flag array.
- The `text` field reads from `SysData:Vote/{N}/text`. Maximum 4095 bytes are read; longer text is truncated.
- If the choices, totals, talley, or text files do not exist, the corresponding fields are empty arrays, zero counts, or `null` -- no error is raised.
- Acquires SEM[14] shared.

---

## `cnet-cli vote results`

### Synopsis
```
cnet-cli vote results <topic-number>
```

### Description
Displays a lightweight results-only view for a vote topic. Returns per-choice vote counts with choice text, total votes, and voter count. Does not include the full topic metadata fields or descriptive text.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<topic-number>` | Yes | 1-based topic number. Must be all digits and >= 1 |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `topic_number` | number | The requested topic number |
| `results` | array | Array of per-choice result objects |
| `total_votes` | number | Sum of all vote counts across all choices |
| `voter_count` | number | Number of distinct users who have voted on this topic |
| `warnings` | array | Present only if there are warnings |

Each element of `results`:

| Field | Type | Description |
|-------|------|-------------|
| `choice` | number | 1-based choice number |
| `text` | string | Choice text (MCI codes stripped) |
| `votes` | number | Number of votes for this choice |

### Error Output

On failure, returns `{"error": "<message>"}` with one of:
- `"No vote topics configured"` -- topics file does not exist
- `"Vote topic N out of range (1-M)"` -- topic number exceeds the number of topics in the file
- `"Topic number must be >= 1"` -- topic number is zero or negative
- `"Failed to read vote topic"` -- read error on the topics file

### Notes
- This command reads the same underlying data files as `vote show` but produces a more compact output focused on results. It omits the topic metadata fields (`name`, `serial`, `float`, `max_choices`, hex fields) and the descriptive `text`.
- The maximum number of choices per topic is 100 (limited by the 100-byte vote flag array in the talley record).
- If the choices or totals files do not exist, the `results` array is empty and `total_votes` is 0.
- Acquires SEM[14] shared.
