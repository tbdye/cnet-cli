# Stats Command

System statistics including counters, SAM (System Activity Monitor) data, and SAG (System Activity Graph) data.

## `cnet-cli stats`

### Synopsis
```
cnet-cli stats
```

### Description
Returns system counters, online user count, boot date, and the full SAM and SAG activity data arrays. SAM and SAG data are copied under SEM[18] shared lock.

### Arguments
None.

### Options
None.

### Output Fields

#### Top-level object

| Field | Type | Description |
|-------|------|-------------|
| `counters` | object | Aggregate system counters |
| `system` | object | System configuration info |
| `boot_date` | string or null | SAM period boot date as ISO 8601 (`YYYY-MM-DDTHH:MM:SS`), or null if unset |
| `sam` | object | System Activity Monitor data |
| `sag` | object | System Activity Graph data |

#### `counters` object

| Field | Type | Description |
|-------|------|-------------|
| `total_accounts` | number | Total account slots (`Nums[NUMS_CURRENT_ACCOUNTS]`) |
| `active_accounts` | number | In-use account slots (`Nums[NUMS_INUSE_ACCOUNTS]`) |
| `highest_id` | number | Highest user ID number (`Nums[NUMS_HIGH_ID]`) |
| `total_calls` | number | Lifetime total calls (`Nums[NUMS_CALLS_TOTAL]`) |
| `calls_logged_now` | number | Currently logged-in sessions (`Nums[NUMS_CALLS_LOGGED]`) |

#### `system` object

| Field | Type | Description |
|-------|------|-------------|
| `subboards` | number | Total subboard count |
| `ports_configured` | number | Number of configured ports |
| `hi_port` | number | Highest port index |
| `open_pfiles` | number | Number of open pfiles |
| `users_online` | number | Count of ports with active online sessions |

#### `sam` object

The SAM (System Activity Monitor) tracks activity counters across five time periods and fifteen activity categories.

| Field | Type | Description |
|-------|------|-------------|
| `row_labels` | array of string | Names for the 5 rows (time periods) |
| `column_labels` | array of string | Names for the 15 columns (activity categories) |
| `data` | array of array of number | 5x15 matrix of counter values |

**Row labels** (time periods):

| Index | Label | Description |
|-------|-------|-------------|
| 0 | `current` | Current session/period counters |
| 1 | `period` | Rolling period counters |
| 2 | `daily` | Today's counters |
| 3 | `total` | All-time totals |
| 4 | `last_call` | Counters from the last call |

**Column labels** (activity categories):

| Index | Label | Description |
|-------|-------|-------------|
| 0 | `feedbacks` | Feedback messages sent |
| 1 | `mail_sent` | Mail messages sent |
| 2 | `sysop_mail` | Sysop mail messages |
| 3 | `posts` | Message board posts |
| 4 | `responses` | Message board responses |
| 5 | `group_changes` | Access group changes |
| 6 | `pfiles` | Pfile executions |
| 7 | `new_users` | New user signups |
| 8 | `upload_files` | Files uploaded |
| 9 | `upload_kb` | KB uploaded |
| 10 | `download_files` | Files downloaded |
| 11 | `download_kb` | KB downloaded |
| 12 | `minutes_used` | Minutes of connect time used |
| 13 | `minutes_idle` | Minutes of idle time |
| 14 | `charges` | Billing charges |

Access `data[row][col]` using the index mappings above. For example, `data[2][3]` is daily posts.

#### `sag` object

The SAG (System Activity Graph) tracks call counts for graph display.

| Field | Type | Description |
|-------|------|-------------|
| `row_labels` | array of string | Names for the 2 rows |
| `data` | array of array of number | 2x72 matrix of unsigned counter values |

**Row labels:**

| Index | Label | Description |
|-------|-------|-------------|
| 0 | `daily` | Daily activity graph data (72 values) |
| 1 | `hourly` | Hourly activity graph data (72 values) |

### Notes
- Read-only.
- SAM and SAG arrays are copied under SEM[18] shared lock to ensure a consistent snapshot. The boot date (`SAMDate[1]`) is also copied under this lock.
- Online user count is computed by iterating all ports (0..HiPort) and counting those with an active session, independent of the semaphore-protected data.
- SAM column labels correspond to the two-character codes used in the callers log SAM summary lines (e.g., `fb`=feedbacks, `ms`=mail_sent, `sm`=sysop_mail, `po`=posts, `re`=responses, `gc`=group_changes, `pf`=pfiles, `nu`=new_users, `uf`=upload_files, `uk`=upload_kb, `df`=download_files, `dk`=download_kb, `mu`=minutes_used, `mi`=minutes_idle, `ch`=charges).
