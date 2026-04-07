# Conference Room Commands

Commands for querying CNet BBS conference rooms. Conference rooms are real-time chat areas that users can join.

## `cnet-cli conf list`

### Synopsis
```
cnet-cli conf list [--all]
```

### Description
Lists conference rooms. By default, only rooms that are either occupied (have users) or marked as permanent are shown. Use `--all` to include empty non-permanent rooms.

### Arguments
None.

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--all` | flag | off | Include all allocated rooms, even empty non-permanent ones |

### Output Fields

Top-level object contains a `rooms` array. Each element:

| Field | Type | Description |
|-------|------|-------------|
| `room_number` | number | Room index (0-99) |
| `name` | string | Room name (MCI codes stripped) |
| `topic` | string | Room topic (MCI codes stripped) |
| `creator` | number | Account number of the room creator |
| `users` | number | Number of users currently in the room |
| `public` | boolean | Whether the room is publicly visible |
| `quiet` | boolean | Whether the room is in quiet mode |
| `permanent` | boolean | Whether the room is permanent (persists when empty) |
| `max_users` | number | Maximum user capacity |
| `channel` | number | Channel number |

### Notes
- Read-only.
- Reads `CRoom[0..99]` under SEM[8] shared lock.
- CNet supports up to 100 conference rooms (indices 0-99). Unallocated room slots (NULL pointers) are always skipped.
- Without `--all`, a room is shown only if `Users > 0` or `PermaRoom` is set.
- The `name` and `topic` fields have MCI codes stripped via `MCIRemove()` from cnet.library.
