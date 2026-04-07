# Config Commands

Config commands display and modify BBS-wide configuration settings. Configuration is stored across several CNet data files and in-memory structures. Passwords (`MyLinkPass`, `SysPassword`, `ppass`) are never emitted in any output for security.

---

## `cnet-cli config show`

### Synopsis
```
cnet-cli config show
```

### Description
Displays the complete BBS global configuration as a single JSON object, organized into logical sections. Reads from the in-memory `MainPort->gc` (`NewConfig1`) and, when available, `MainPort->MPE->gc2` (`ConfigExtension`) structures.

This is a read-only command.

### Arguments
None.

### Options
None.

### Output Fields

The output is a single JSON object with the following nested sections:

#### `identity` object

| Field | Type | Description |
|-------|------|-------------|
| `system_name` | string | BBS system name (MCI codes stripped) |
| `sysop_name` | string | Sysop name (MCI codes stripped) |
| `location` | string | BBS location |
| `phone_number` | string | BBS phone number |
| `bbs_id` | string | BBS ID string |
| `country` | string | Country |
| `area_code` | string | Telephone area code |
| `uucp_name` | string | UUCP name |
| `link_id` | number | Link ID number |

#### `limits` object

| Field | Type | Description |
|-------|------|-------------|
| `max_user_accounts` | number | Maximum number of user accounts |
| `max_open_pfiles` | number | Maximum simultaneously open pfiles/doors |
| `num_rooms` | number | Number of conference rooms |
| `max_link_ports` | number | Maximum link ports |
| `max_subboards` | number | Maximum number of subboards |
| `max_select` | number | Maximum select count |
| `max_upload` | number | Maximum upload count |
| `max_list` | number | Maximum list count |
| `max_logon_attempts` | number | Maximum logon attempts before disconnect |
| `max_logon_time` | number | Maximum logon time in minutes |
| `max_yank_tasks` | number | Maximum concurrent yank tasks |
| `max_yank_size` | number | Maximum yank size |
| `max_yank_days` | number | Maximum yank age in days |
| `max_yanks_per_user` | number | Maximum yanks per user |
| `max_short_lines` | number | Maximum short message lines |
| `abuffer_size` | number | A-buffer size (unsigned) |
| `max_file_process` | number | Maximum file process count (unsigned) |

#### `defaults` object

| Field | Type | Description |
|-------|------|-------------|
| `balance` | number | Default account balance |
| `net_credits` | number | Default network credits |
| `byte_credits` | number | Default byte credits |
| `file_credits` | number | Default file credits |
| `door_points` | number | Default door points |
| `time_form` | number | Default time display format |
| `default_color` | number | Default color code |
| `default_protocol` | string | Default file transfer protocol |
| `mail_sort` | number | Default mail sort order |

#### `paths` object

| Field | Type | Description |
|-------|------|-------------|
| `olm` | string | OLM (online message) path |
| `zip` | string | Archive/ZIP path |
| `extract` | string | Extract path |
| `yank_work` | string | Yank work directory |
| `ram` | string | RAM path |
| `terminal` | string | Terminal path |
| `local_editor` | string | Local editor path |
| `cdrom` | string | CD-ROM path |
| `dictionary` | string | Dictionary path |
| `outbound` | string | FidoNet outbound path |
| `inbound` | string | FidoNet inbound path |
| `ram_upload` | string | RAM upload path |
| `disk_upload` | string | Disk upload path |
| `nodelist` | string | FidoNet nodelist path |
| `news` | string | News path |
| `uumail` | string | UUCP mail path |

#### `options` object

| Field | Type | Description |
|-------|------|-------------|
| `logon_feedback` | boolean | Prompt for feedback at logon |
| `logon_search` | boolean | Enable logon search |
| `guest_users` | boolean | Allow guest user access |
| `hide_status` | boolean | Hide status bar |
| `conf_profile` | boolean | Show conference profile |
| `mail_feedback` | boolean | Mail feedback enabled |
| `separate_texts` | boolean | Use separate text files |
| `indent_spaces` | number | Number of indent spaces |
| `skip_idle_ports` | boolean | Skip idle ports in listings |
| `blank_ticks` | number | Screen blank timer in ticks |
| `blank_bright` | number | Screen blank brightness level |
| `blist_purge_days` | number | BBS list purge interval in days |
| `mid_from_handle` | boolean | Generate message ID from handle |
| `file_task_notify` | boolean | Notify on file task completion |
| `monitor_uumail` | boolean | Monitor UUCP mail |
| `create_web_dir` | boolean | Auto-create web directories |
| `news_task_post` | boolean | News task posting enabled |
| `dynamic_ip` | boolean | Dynamic IP addressing enabled |
| `force_empty_trash` | boolean | Force empty trash on logoff |

#### `resource_counts` object

| Field | Type | Description |
|-------|------|-------------|
| `archivers` | number | Number of configured archivers |
| `editors` | number | Number of configured editors |
| `protocols` | number | Number of configured protocols |
| `fido_networks` | number | Number of FidoNet networks |
| `log_types` | number | Number of log types |

#### `network` object

Fields from `NewConfig1`:

| Field | Type | Description |
|-------|------|-------------|
| `news_server` | string | News server hostname |
| `nntp_port` | number | NNTP port number |
| `root_name` | string | Root domain name |
| `ram_upload_size` | number | RAM upload size limit (unsigned) |

Additional fields from `ConfigExtension` (present only when `MainPort->MPE` is available):

| Field | Type | Description |
|-------|------|-------------|
| `mail_server` | string | SMTP mail server hostname |
| `smtp_mail` | boolean | SMTP mail enabled |
| `mail_timeout` | number | Mail connection timeout (unsigned) |
| `news_timeout` | number | News connection timeout (unsigned) |
| `timezone` | string | Timezone string |
| `smtpd_timeout` | number | SMTP daemon timeout (unsigned) |
| `port_log_dir` | string | Port log directory path |
| `smtpd_temp_dir` | string | SMTP daemon temp directory |
| `user_cache` | number | User cache size |
| `telnetd_autoload` | boolean | Auto-load telnet daemon |
| `max_telnetd` | number | Maximum telnet daemon instances |
| `show_ip_where` | boolean | Show IP in "where" display |
| `min_telnetd_mem_kb` | number | Minimum memory (KB) for telnet daemon (unsigned) |
| `next_sub_serial` | number | Next subboard serial number (unsigned) |

#### `task_buffer_limits` object

All fields are unsigned numbers. Present only when `MainPort->MPE` is available; otherwise the object is empty.

| Field | Type | Description |
|-------|------|-------------|
| `mail_task` | number | Mail task buffer maximum |
| `news_task` | number | News task buffer maximum |
| `file_task` | number | File task buffer maximum |
| `yank_task` | number | Yank task buffer maximum |
| `smtpd` | number | SMTP daemon buffer maximum |
| `telnetd` | number | Telnet daemon buffer maximum |
| `ftpd` | number | FTP daemon buffer maximum |

### Notes
- The `network` and `task_buffer_limits` sections include extended fields only when `MainPort->MPE` (MainPortExtension) is present. On older CNet builds without MPE, only the base `NewConfig1` fields appear in `network`, and `task_buffer_limits` is empty.
- Passwords are never included in the output.

---

## `cnet-cli config flags`

### Synopsis
```
cnet-cli config flags [--set <flag>=<value> ...]
```

### Description
Reads or modifies the BBS control panel toggle flags. Without `--set`, displays the current flag values (read-only). With one or more `--set` arguments, modifies the specified flags, updates both in-memory state and the on-disk `cnet:bbscontrol3` file, and returns the new values.

The flags correspond to the CNet Control Panel toggles that control BBS operational modes (doors closed, files closed, messages closed, new user registration, sysop presence).

### Arguments
None (positional).

### Options
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--set` | flag=value | (none) | Set a flag to a value. Can be repeated for multiple flags. Value must be `true`, `false`, `1`, or `0`. |

### Available Flags

| Flag Name | Bit | Description |
|-----------|-----|-------------|
| `doors_closed` | 0 | Doors/pfiles are closed to users |
| `files_closed` | 1 | File transfer areas are closed to users |
| `msgs_closed` | 2 | Message areas are closed to users |
| `no_new_users` | 3 | New user registration is disabled |
| `sysop_in` | 4 | Sysop is marked as present |

### Output Fields

**Read-only mode** (no `--set`):

| Field | Type | Description |
|-------|------|-------------|
| `flags` | object | Object containing all flag name/boolean pairs |

The `flags` object contains one boolean field per flag (see Available Flags above).

**Write mode** (with `--set`):

| Field | Type | Description |
|-------|------|-------------|
| `flags` | object | Object containing all flag name/boolean pairs (after update) |
| `updated` | boolean | Always `true` when write succeeds |
| `warnings` | array | (Optional) Array of warning strings if disk persistence failed |

### Notes
- Write mode updates `MainPort->pc[0].check` in memory and sets `MainPort->check_latch` to signal CNet to pick up the change.
- Write mode also persists the change to `cnet:bbscontrol3` on disk. If disk write fails (seek, write, or open failure), a warning is included but the in-memory change still takes effect until the next reboot.
- Multiple `--set` arguments can be provided in a single invocation to change several flags atomically.
- The `--set` argument format is `flag=value` with no spaces around the `=` sign. For example: `--set doors_closed=true --set sysop_in=false`.

---

## `cnet-cli config reload-text`

### Synopsis
```
cnet-cli config reload-text
```

### Description
Triggers a reload of the BBSTEXT and BBSMENU text files. Sets the `reload_text` flag in the `MainPortExtension` under SEM[0] exclusive lock, which CNet will pick up and process on its next cycle.

This is a write command (it triggers a side effect in the running BBS).

### Arguments
None.

### Options
None.

### Output Fields

| Field | Type | Description |
|-------|------|-------------|
| `action` | string | Always `"reload-text"` |
| `status` | string | Always `"triggered"` |

### Notes
- Requires `MainPort->MPE` (MainPortExtension) to be available. Returns an error if MPE is not present.
- The reload is asynchronous -- the command returns immediately after setting the flag. The actual reload happens when CNet processes the flag.
- Acquires MPE sem[0] exclusive for the flag write.

---

## `cnet-cli config port`

### Synopsis
```
cnet-cli config port <port-number>
```

### Description
Displays per-port configuration for a specific BBS port. Handles both loaded (active in memory) and unloaded (on disk only) ports. For loaded ports, reads configuration from the in-memory `MainPort->pc[]` and `PortData->PDE->sp` structures. For unloaded ports, reads the `PortConfig` from `cnet:bbscontrol3` and the `SerPort4` from `cnet:configs/bbsportN` on disk.

This is a read-only command.

### Arguments
| Argument | Required | Description |
|----------|----------|-------------|
| `<port-number>` | Yes | Port number (0-99) |

### Options
None.

### Output Fields

Top-level object:

| Field | Type | Description |
|-------|------|-------------|
| `port` | number | Port number |
| `loaded` | boolean | Whether the port is currently loaded in memory |
| `port_config` | object | Port configuration (see below) |
| `serial_config` | object/null | Serial/device configuration, or `null` if unavailable |
| `warnings` | array | (Optional) Array of warning strings |

#### `port_config` object

| Field | Type | Description |
|-------|------|-------------|
| `online` | boolean | Port is marked online |
| `screen_open` | string | Screen open mode: `"none"`, `"permanent"`, `"forcall"`, or `"workbench"` |
| `check` | number | Raw check byte value (control panel flags for this port) |
| `idle` | number | Idle timeout setting |
| `offline` | boolean | Port is marked offline |
| `bplanes` | number | Number of bitplanes for the port screen |
| `interlace` | string | Interlace mode: `"none"`, `"24-line"`, or `"49-line"` |

#### `serial_config` object

| Field | Type | Description |
|-------|------|-------------|
| `device_name` | string | AmigaOS device name (e.g., `"a2065.device"`) |
| `unit` | number | Device unit number |
| `flags` | number | Device open flags |
| `idle_baud` | number | Idle baud rate |
| `escape` | number | Escape character code |
| `answer_pause` | number | Answer pause duration |
| `seconds` | number | Timeout in seconds |
| `init1` | string | Modem init string 1 |
| `init2` | string | Modem init string 2 |
| `hangup` | string | Modem hangup string |
| `dialout` | string | Modem dialout string |
| `answer` | string | Modem answer string |
| `offhook` | string | Modem off-hook string |
| `terminal` | string | Terminal type string |
| `caller_id` | string | Caller ID string |
| `ring` | string | Ring detection string |
| `connect` | string | Connect string |
| `termlink` | string | Terminal link string |
| `null_modem` | boolean | Null modem mode (direct serial connection) |
| `idle_who` | string | Idle "who" display string |
| `port_flags` | object | Port flag sub-object (see below) |

#### `port_flags` sub-object (within `serial_config`)

| Field | Type | Description |
|-------|------|-------------|
| `show_on_who` | boolean | Show this port in "who's online" listings |
| `telnetd` | boolean | Port operates as a telnet daemon |
| `offclose` | boolean | Close port when going offline |

### Notes
- A port is considered "loaded" if its port number is within `MainPort->HiPort` and its `PortData` pointer is non-null and distinct from `MainPort->z0`.
- For unloaded ports, the `PortConfig` is read from `cnet:bbscontrol3` at the appropriate offset (port_number * sizeof(PortConfig)).
- For unloaded ports, the `SerPort4` is read from `cnet:configs/bbsportN` where N is the port number. If the file does not exist or is incomplete, `serial_config` is `null` and a warning is emitted.
- For loaded ports, if the `PDE` (PortDataExtension) is unavailable, `serial_config` is `null` with a warning.
- The `ppass` field from `SerPort4` is intentionally omitted from the output for security.
