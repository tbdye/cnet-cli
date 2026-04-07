# cnet-cli Command Reference

Quick reference for all cnet-cli commands. For full details (options, output fields, notes), see the linked command group documentation.

## How to Use This Reference

1. Find the command you need in the table below.
2. Follow the link to the command group doc for full synopsis, options, and output fields.
3. All commands output JSON and are invoked via `amigactl exec "C:cnet-cli <command>"`.

## Commands

### System ([system.md](commands/system.md))
| Command | Description | Type |
|---------|-------------|------|
| `status` | Show BBS status, version, and counters | Read |
| `ports` | List all ports with online status | Read |
| `who` | List online users (basic) | Read |
| `who --detail` | List online users (extended) | Read |
| `who <port>` | Show single port's online user detail | Read |

### Subboards ([sub.md](commands/sub.md))
| Command | Description | Type |
|---------|-------------|------|
| `sub list` | List subboards with optional filters | Read |
| `sub show <id>` | Show full subboard detail | Read |
| `sub tree` | Show subboard hierarchy as a tree | Read |
| `sub path <id>` | Show subboard ancestry to root | Read |
| `sub disk-usage <id>` | Report data directory disk usage | Read |
| `sub create` | Create a new subboard | Write |
| `sub edit <id>` | Modify subboard fields | Write |
| `sub delete <id>` | Mark subboard as killed | Write |

### Messages ([msg.md](commands/msg.md))
| Command | Description | Type |
|---------|-------------|------|
| `msg list <sub>` | List message items in a subboard | Read |
| `msg read <sub> <num>` | Read message with text and responses | Read |
| `msg post <sub>` | Post a new message | Write |
| `msg respond <sub> <num>` | Add a response to a message | Write |
| `msg delete <sub> <num>` | Mark a message as killed | Write |
| `msg edit <sub> <num>` | Edit message text or title | Write |
| `msg search <query>` | Search messages across subboards | Read |
| `msg move <src> <num> <dst>` | Move a message between subboards | Write |

### Users ([user.md](commands/user.md))
| Command | Description | Type |
|---------|-------------|------|
| `user list` | List user accounts | Read |
| `user show <acct>` | Show full user detail | Read |
| `user find <query>` | Search users by handle, name, or phone | Read |
| `user plan <acct>` | Read user's plan file | Read |
| `user profile <acct>` | Show public user profile | Read |
| `user edit <acct>` | Modify user fields | Write |
| `user disable <acct>` | Suspend a user account | Write |
| `user enable <acct>` | Unsuspend a user account | Write |
| `user delete <acct>` | Permanently delete a user account | Write |
| `olm <port>` | Send an On-Line Message to a port | Write |

### Mail ([mail.md](commands/mail.md))
| Command | Description | Type |
|---------|-------------|------|
| `mail send` | Send a mail message | Write |
| `mail list <acct>` | List mail headers in a folder | Read |
| `mail read <acct> <num>` | Read a mail message with body | Read |
| `mail reply <acct> <num>` | Reply to a mail message | Write |
| `mail delete <acct> <num>` | Delete a mail message | Write |
| `mail folders <acct>` | List mail folders with counts | Read |
| `mail count <acct>` | Count messages in a folder | Read |
| `mail feedback` | Sysop feedback mail shortcut | Read |
| `mail verify <acct>` | View sent mail shortcut | Read |
| `mail alias list <acct>` | List mail aliases | Read |
| `mail alias add <acct>` | Add a mail alias | Write |
| `mail alias remove <acct>` | Remove a mail alias | Write |

### Files ([file.md](commands/file.md))
| Command | Description | Type |
|---------|-------------|------|
| `file list <sub>` | List files in a file area | Read |
| `file show <sub> <num>` | Show full file entry detail | Read |
| `file add <sub>` | Register a file in the catalog | Write |
| `file edit <sub> <num>` | Edit file entry flags | Write |
| `file remove <sub> <num>` | Kill a file entry | Write |
| `file validate <sub> <range>` | Batch-validate file entries | Write |
| `file find <query>` | Search files across subboards | Read |
| `file missing` | Audit for missing/restored files | Read/Write |

### News ([news.md](commands/news.md))
| Command | Description | Type |
|---------|-------------|------|
| `news list <sub>` | List items in a text/door area | Read |
| `news read <sub> <num>` | Read news item with text | Read |
| `news post <sub>` | Create a news item | Write |
| `news edit <sub> <num>` | Edit news item text or title | Write |
| `news delete <sub> <num>` | Kill a news item | Write |

### GFiles ([gfile.md](commands/gfile.md))
| Command | Description | Type |
|---------|-------------|------|
| `gfile list <sub>` | List items in a GFile area | Read |
| `gfile read <sub> <num>` | Read GFile item with text | Read |
| `gfile add <sub>` | Add a new GFile item | Write |
| `gfile remove <sub> <num>` | Kill a GFile item | Write |

### Access Groups ([group.md](commands/group.md))
| Command | Description | Type |
|---------|-------------|------|
| `group list` | List all 32 access groups | Read |
| `group show <id>` | Show group detail with privileges | Read |
| `group edit <id>` | Modify group fields and privileges | Write |
| `group transpose <id>` | Push group privileges to all members | Write |

### Configuration ([config.md](commands/config.md))
| Command | Description | Type |
|---------|-------------|------|
| `config show` | Show full BBS configuration | Read |
| `config flags` | Read/write control panel toggle flags | Read/Write |
| `config reload-text` | Trigger BBSTEXT/BBSMENU reload | Write |
| `config port <N>` | Show per-port configuration | Read |

### Ports ([port.md](commands/port.md))
| Command | Description | Type |
|---------|-------------|------|
| `port load <N>` | Load (start) a BBS port | Write |
| `port unload <N>` | Unload (stop) a BBS port | Write |
| `port dump <N>` | Disconnect user on a port | Write |

### Statistics ([stats.md](commands/stats.md))
| Command | Description | Type |
|---------|-------------|------|
| `stats` | System counters, SAM, and SAG data | Read |

### Logs ([log.md](commands/log.md))
| Command | Description | Type |
|---------|-------------|------|
| `log list` | List log files with sizes and dates | Read |
| `log read <name>` | Read a log file as JSON lines | Read |
| `log callers` | Read callers log (raw) | Read |
| `log callers-parsed` | Parse callers log into records | Read |

### ARexx ([arexx.md](commands/arexx.md))
| Command | Description | Type |
|---------|-------------|------|
| `arexx send <port> <cmd>` | Send ARexx command to a port | Read/Write |
| `arexx control <cmd>` | Send ARexx command to control panel | Read/Write |

### Conference Rooms ([conf.md](commands/conf.md))
| Command | Description | Type |
|---------|-------------|------|
| `conf list` | List conference rooms | Read |

### Events ([event.md](commands/event.md))
| Command | Description | Type |
|---------|-------------|------|
| `event list` | List scheduled events | Read |
| `event show <index>` | Show event detail | Read |

### Maintenance ([maint.md](commands/maint.md))
| Command | Description | Type |
|---------|-------------|------|
| `maint pointers` | Rebuild user index files | Write |
| `maint count` | Recount subboard/system counters | Read/Write |
| `maint repair-mail` | Compact mail data files | Read/Write |
| `maint repair-sub` | Compact subboard text pool (stub) | -- |

### BBS List ([bbslist.md](commands/bbslist.md))
| Command | Description | Type |
|---------|-------------|------|
| `bbslist list` | List BBS directory entries | Read |

### Voting ([vote.md](commands/vote.md))
| Command | Description | Type |
|---------|-------------|------|
| `vote list` | List vote topics | Read |
| `vote show <number>` | Show topic detail with choices | Read |
| `vote results <number>` | Show vote results only | Read |

---

**Total: 89 commands** across 19 functional areas, backed by 22 entries in the top-level dispatch table.

All commands are verified present in both the source dispatch tables (`src/main.c`) and the corresponding documentation files (`docs/commands/*.md`).
