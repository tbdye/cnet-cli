/*
 * cnet-cli -- CNet BBS standalone admin CLI
 *
 * System status, port listing, online users.
 *
 * Build:
 *   make CNET_SDK_PATH=../cnet-sdk
 *
 * Deploy + run via amigactl:
 *   amigactl put cnet-cli "T:cnet-cli"
 *   amigactl exec "T:cnet-cli status"
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

/*
 * CNet SDK master include.
 *
 * The SDK now uses CNET_PACK_BEGIN/CNET_PACK_END from cnet/align.h for
 * struct alignment. On m68k, these expand to nothing -- GCC's natural
 * 2-byte alignment matches SAS/C. No #define packed workaround needed.
 */
#include <cnet/cnet.h>
#include <cnet/eventdefs.h>

/*
 * cnet.h redefines __asm to nothing for SAS/C compatibility.
 * Undo this before including AmigaOS proto headers, which need
 * __asm as a GCC keyword for __REG() register macros.
 */
#undef __asm

#include <proto/exec.h>
#include <proto/cnet4.h>

/*
 * Compile-time verification that struct sizes match the live SAS/C ABI.
 * These sizes are confirmed by cnet_probe3.c against the running system.
 */
_Static_assert(sizeof(struct SubboardType4) == 696,
    "SubboardType4 must be 696 bytes (SAS/C alignment)");
_Static_assert(sizeof(struct KeyElement4) == 74,
    "KeyElement4 must be 74 bytes (SAS/C alignment)");
_Static_assert(sizeof(struct UserData) == 672,
    "UserData must be 672 bytes");
_Static_assert(sizeof(struct ItemType3) == 168,
    "ItemType3 must be 168 bytes");
_Static_assert(sizeof(struct ItemHeader) == 34,
    "ItemHeader must be 34 bytes");
_Static_assert(sizeof(struct MessageType3) == 28,
    "MessageType3 must be 28 bytes");
_Static_assert(sizeof(struct HeaderType) == 288,
    "HeaderType must be 288 bytes (SAS/C alignment)");
_Static_assert(sizeof(struct AccessGroup) == 156,
    "AccessGroup must be 156 bytes");
_Static_assert(sizeof(struct Privs) == 92,
    "Privs must be 92 bytes");
_Static_assert(sizeof(struct RoomConfig) == 628,
    "RoomConfig size mismatch -- check packing");
_Static_assert(sizeof(struct Room) == 16662,
    "Room size mismatch -- check packing");
_Static_assert(sizeof(struct PortConfig) == 24,
    "PortConfig must be 24 bytes");
_Static_assert(sizeof(struct SerPort4) == 492,
    "SerPort4 size mismatch with on-disk format");
_Static_assert(sizeof(struct JobType4) == 186,
    "JobType4 must be 186 bytes (SAS/C alignment)");

#include <rexx/rxslib.h>

#include "json.h"
#include "util.h"
#include "subboard.h"
#include "message.h"
#include "user.h"
#include "mail.h"
#include "file.h"
#include "news.h"
#include "gfile.h"
#include "group.h"
#include "bbsconfig.h"
#include "stats.h"
#include "log.h"
#include "arexx.h"
#include "port.h"
#include "conf.h"
#include "events.h"
#include "maint.h"
#include "bbslist.h"
#include "vote.h"
#include "alias.h"

/* Stack size for libnix -- 64KB is generous for our needs. */
unsigned long __stack = 65536;

/*
 * CNetBase: required by the inline stubs in <inline/cnet.h>.
 * proto/cnet.h declares "extern struct Library *CNetBase" and the
 * inline macros reference it by name. We provide the actual storage.
 */
struct Library *CNetBase = NULL;

/*
 * CNetMailBase: required by the inline stubs in <inline/cnetmail.h>.
 * proto/cnetmail.h (included via cnet/cnet.h) declares
 * "extern struct Library *CNetMailBase". We provide the actual storage.
 * May be NULL if cnetmail.library is not available.
 */
struct Library *CNetMailBase = NULL;

/*
 * RexxSysBase: required by the inline stubs in <inline/rexxsyslib.h>.
 * proto/rexxsyslib.h (via <inline/rexxsyslib.h>) references RexxSysBase
 * by name (#define REXXSYSLIB_BASE_NAME RexxSysBase). We provide the
 * actual storage. May be NULL if rexxsyslib.library is not available.
 */
struct RxsLib *RexxSysBase = NULL;

/*
 * CNet4Base: required by the inline stubs in <inline/cnet4.h>.
 * proto/cnet4.h declares "extern struct Library *CNet4Base".
 * We provide the actual storage. May be NULL if cnet4.library
 * is not available.
 */
struct Library *CNet4Base = NULL;

/* Global MainPort pointer, valid after init_cnet(). */
static struct MainPort *myp = NULL;

/*
 * Global argc/argv, set in main() before command dispatch.
 * Used by two-level dispatch (e.g., cmd_sub) to forward
 * remaining arguments to subcommand handlers.
 */
int g_argc;
char **g_argv;

/* ---------- command: status ---------- */

static int cmd_status(void)
{
    struct json_state js;
    char buf[128];

    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_str(&js, "system_name",
        strip_mci(buf, sizeof(buf), myp->gc.MySystemName));
    json_kv_str(&js, "sysop_name",
        strip_mci(buf, sizeof(buf), myp->gc.MySysopName));
    json_kv_int(&js, "version", myp->s1);
    json_kv_int(&js, "serial", myp->s3);
    json_kv_str(&js, "registered_to",
        strip_mci(buf, sizeof(buf), myp->regto));
    json_kv_int(&js, "ports", (long)myp->nPorts);
    json_kv_int(&js, "hi_port", (long)myp->HiPort);
    json_kv_int(&js, "accounts", myp->Nums[NUMS_CURRENT_ACCOUNTS]);
    json_kv_int(&js, "total_calls", myp->Nums[NUMS_CALLS_TOTAL]);
    json_kv_int(&js, "logged_now", myp->Nums[NUMS_CALLS_LOGGED]);
    json_kv_int(&js, "subboards", myp->ns);
    json_kv_int(&js, "root_sub", (long)myp->root);
    json_kv_int(&js, "open_pfiles", myp->OpenPfiles);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---------- command: ports ---------- */

static int cmd_ports(void)
{
    struct json_state js;
    char buf[64];
    int i;

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "ports");
    json_arr_open(&js);

    for (i = 0; i <= (int)myp->HiPort && i < 100; i++) {
        struct PortData *z = myp->PortZ[i];
        int loaded;

        /*
         * Unloaded ports point to myp->z0 (the default PortData),
         * not NULL. Both checks for safety.
         */
        loaded = (z && z != myp->z0);

        json_obj_open(&js);
        json_kv_int(&js, "port", (long)i);
        json_kv_bool(&js, "loaded", loaded);

        if (loaded) {
            json_kv_bool(&js, "online", (int)z->OnLine);

            if (z->OnLine) {
                json_kv_str(&js, "user",
                    strip_mci(buf, sizeof(buf), z->user1.Handle));
                json_kv_int(&js, "account", (long)z->id);
                json_kv_uint(&js, "baud", (unsigned long)z->user1.BaudRate);
                json_kv_uint(&js, "idle_tenths",
                    (unsigned long)z->TimeIdle);
            } else {
                json_kv_null(&js, "user");
                json_kv_uint(&js, "baud", 0);
            }
        } else {
            json_kv_bool(&js, "online", 0);
            json_kv_null(&js, "user");
            json_kv_uint(&js, "baud", 0);
        }

        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---------- command: who ---------- */

static int cmd_who(void)
{
    struct json_state js;
    char buf[64];
    int i;

    /*
     * Route "who --detail" and "who <port>" to
     * cmd_who_detail for extended output.
     */
    if (g_argc >= 3) {
        int who_argc = g_argc - 1;
        char **who_argv = g_argv + 1;
        return cmd_who_detail(myp, who_argc, who_argv);
    }

    /* Original simple who output (g_argc == 2) */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "users");
    json_arr_open(&js);

    for (i = 0; i <= (int)myp->HiPort && i < 100; i++) {
        struct PortData *z = myp->PortZ[i];

        if (!z || z == myp->z0) continue;
        if (!z->OnLine) continue;

        json_obj_open(&js);

        json_kv_str(&js, "handle",
            strip_mci(buf, sizeof(buf), z->user1.Handle));
        json_kv_int(&js, "port", (long)z->InPort);
        json_kv_int(&js, "account", (long)z->id);

        /*
         * Location: z->MyDoing is a pointer to a string describing
         * what the user is currently doing. It may be NULL.
         * Also try z->Doing (a fixed buffer) as fallback.
         */
        if (z->MyDoing && z->MyDoing[0]) {
            json_kv_str(&js, "location",
                strip_mci(buf, sizeof(buf), z->MyDoing));
        } else if (z->Doing[0]) {
            json_kv_str(&js, "location",
                strip_mci(buf, sizeof(buf), z->Doing));
        } else {
            json_kv_null(&js, "location");
        }

        /*
         * TimeIdle and TimeOnLine are in tenths of minutes.
         * Convert to whole minutes for the JSON output.
         */
        json_kv_int(&js, "idle_minutes",
            (long)(z->TimeIdle / 10));
        json_kv_int(&js, "time_online_minutes",
            (long)(z->TimeOnLine / 10));

        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---------- command: sub (subboard dispatch) ---------- */

struct sub_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct sub_command sub_commands[] = {
    { "list",       cmd_sub_list       },
    { "show",       cmd_sub_show       },
    { "tree",       cmd_sub_tree       },
    { "path",       cmd_sub_path       },
    { "disk-usage", cmd_sub_disk_usage },
    { "create",     cmd_sub_create     },
    { "edit",       cmd_sub_edit       },
    { "delete",     cmd_sub_delete     },
    { NULL,         NULL               }
};

static int cmd_sub(void)
{
    const struct sub_command *sc;
    int sub_argc;
    char **sub_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli sub <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list [--active] [--type msg|file|door]\n"
            "  show <id|gokey>\n"
            "  tree\n"
            "  path <id|gokey>\n"
            "  disk-usage <id|gokey>\n"
            "  create --title <t> --go <key> --type <type> --parent <id|gokey>\n"
            "         [--data-path <path>] [--access <hex|groups>] [--max-items N]\n"
            "  edit <id|gokey> [--title <t>] [--go <key>] [--type <type>] ...\n"
            "  delete <id|gokey> [--force]");
        return 1;
    }

    /* sub_argv[0] = subcommand name, sub_argv[1..] = remaining args */
    sub_argc = g_argc - 2;
    sub_argv = g_argv + 2;

    for (sc = sub_commands; sc->name; sc++) {
        if (strcmp(sub_argv[0], sc->name) == 0)
            return sc->handler(myp, sub_argc, sub_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown sub command: %s", sub_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: msg (message dispatch) ---------- */

struct msg_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct msg_command msg_commands[] = {
    { "list",    cmd_msg_list    },
    { "read",    cmd_msg_read    },
    { "post",    cmd_msg_post    },
    { "respond", cmd_msg_respond },
    { "delete",  cmd_msg_delete  },
    { "edit",    cmd_msg_edit    },
    { "search",  cmd_msg_search  },
    { "move",    cmd_msg_move    },
    { NULL,      NULL            }
};

static int cmd_msg(void)
{
    const struct msg_command *mc;
    int msg_argc;
    char **msg_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli msg <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list <sub-id|gokey> [--limit N] [--offset N]\n"
            "  read <sub-id|gokey> <item-number>\n"
            "  post <sub-id|gokey> --title <t> --author <acct>"
            " --text <t> [--to <acct>]\n"
            "  respond <sub-id|gokey> <item-number>"
            " --author <acct> --text <t> [--to <acct>]\n"
            "  delete <sub-id|gokey> <item-number>");
        return 1;
    }

    /* msg_argv[0] = subcommand name, msg_argv[1..] = remaining args */
    msg_argc = g_argc - 2;
    msg_argv = g_argv + 2;

    for (mc = msg_commands; mc->name; mc++) {
        if (strcmp(msg_argv[0], mc->name) == 0)
            return mc->handler(myp, msg_argc, msg_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown msg command: %s", msg_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: user (user dispatch) ---------- */

struct user_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct user_command user_commands[] = {
    { "list",    cmd_user_list    },
    { "show",    cmd_user_show    },
    { "find",    cmd_user_find    },
    { "plan",    cmd_user_plan    },
    { "edit",    cmd_user_edit    },
    { "disable", cmd_user_disable },
    { "enable",  cmd_user_enable  },
    { "profile", cmd_user_profile },
    { "delete",  cmd_user_delete  },
    { NULL,      NULL             }
};

static int cmd_user(void)
{
    const struct user_command *uc;
    int user_argc;
    char **user_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli user <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list [--group N] [--limit N] [--offset N]\n"
            "  show <account|handle>\n"
            "  find <query> [--phone]  Search users by handle/name or phone\n"
            "  plan <account|handle>\n"
            "  edit <account|handle> [--handle H] [--realname N] ...\n"
            "  disable <account|handle>\n"
            "  enable <account|handle>\n"
            "  profile <account|handle>\n"
            "  delete <account|handle> --force");
        return 1;
    }

    user_argc = g_argc - 2;
    user_argv = g_argv + 2;

    for (uc = user_commands; uc->name; uc++) {
        if (strcmp(user_argv[0], uc->name) == 0)
            return uc->handler(myp, user_argc, user_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown user command: %s", user_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: mail alias (third-level dispatch) ---------- */

struct alias_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct alias_command alias_commands[] = {
    { "list",   cmd_mail_alias_list   },
    { "add",    cmd_mail_alias_add    },
    { "remove", cmd_mail_alias_remove },
    { NULL,     NULL                  }
};

/*
 * Third-level dispatch for mail alias commands.
 * Called from cmd_mail with mail_argv[0]="alias".
 * Shifts argv by 1 so alias sub-handlers get
 * argv[0]="list"/"add"/"remove", argv[1..]=remaining args.
 */
static int cmd_mail_alias(struct MainPort *mp, int argc, char **argv)
{
    const struct alias_command *ac;
    int alias_argc;
    char **alias_argv;

    if (argc < 2) {
        json_error(
            "Usage: cnet-cli mail alias <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list <account|handle>\n"
            "  add <account|handle> --alias <name>"
            " --name <recipient> [--address <addr>]\n"
            "  remove <account|handle> --alias <name>"
            " [--name <recipient>]");
        return 1;
    }

    /* alias_argv[0] = "list"/"add"/"remove", [1..] = remaining */
    alias_argc = argc - 1;
    alias_argv = argv + 1;

    for (ac = alias_commands; ac->name; ac++) {
        if (strcmp(alias_argv[0], ac->name) == 0)
            return ac->handler(mp, alias_argc, alias_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown mail alias command: %s", alias_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: mail (mail dispatch) ---------- */

struct mail_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct mail_command mail_commands[] = {
    { "send",     cmd_mail_send     },
    { "list",     cmd_mail_list     },
    { "read",     cmd_mail_read     },
    { "reply",    cmd_mail_reply    },
    { "delete",   cmd_mail_delete   },
    { "folders",  cmd_mail_folders  },
    { "count",    cmd_mail_count    },
    { "feedback", cmd_mail_feedback },
    { "verify",   cmd_mail_verify   },
    { "alias",    cmd_mail_alias    },
    { NULL,       NULL              }
};

static int cmd_mail(void)
{
    const struct mail_command *mc;
    int mail_argc;
    char **mail_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli mail <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  send --from <user> --to <user> --subject <s>"
            " --body <text>\n"
            "  list <account|handle> [--folder <name>]"
            " [--limit N] [--offset N]\n"
            "  read <account|handle> <num> [--folder <name>]\n"
            "  reply <account|handle> <num>"
            " --body <text> [--folder <name>]\n"
            "  delete <account|handle> <num>"
            " [--folder <name>]\n"
            "  folders <account|handle>\n"
            "  count <account|handle> [--folder <name>]\n"
            "  feedback [<num>] [--folder <name>]"
            " [--limit N] [--offset N]\n"
            "  verify <account|handle>"
            " [--limit N] [--offset N]\n"
            "  alias list <account|handle>\n"
            "  alias add <account|handle> --alias <name>"
            " --name <recipient> [--address <addr>]\n"
            "  alias remove <account|handle> --alias <name>"
            " [--name <recipient>]");
        return 1;
    }

    mail_argc = g_argc - 2;
    mail_argv = g_argv + 2;

    for (mc = mail_commands; mc->name; mc++) {
        if (strcmp(mail_argv[0], mc->name) == 0)
            return mc->handler(myp, mail_argc, mail_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown mail command: %s", mail_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: file (file area dispatch) ---------- */

struct file_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct file_command file_commands[] = {
    { "list",     cmd_file_list     },
    { "show",     cmd_file_show     },
    { "add",      cmd_file_add      },
    { "edit",     cmd_file_edit     },
    { "remove",   cmd_file_remove   },
    { "validate", cmd_file_validate },
    { "find",     cmd_file_find     },
    { "missing",  cmd_file_missing  },
    { NULL,       NULL              }
};

static int cmd_file(void)
{
    const struct file_command *fc;
    int file_argc;
    char **file_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli file <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list <sub-id|gokey> [--limit N] [--offset N]\n"
            "  show <sub-id|gokey> <item-number>\n"
            "  add <sub-id|gokey> --title <filename> --author <acct>"
            " [--desc <d>]\n"
            "  edit <sub-id|gokey> <item-number>"
            " [--validated N] [--frozen N] ...\n"
            "  remove <sub-id|gokey> <item-number>"
            " [--delete-physical]\n"
            "  validate <sub-id|gokey> <range>\n"
            "  find <query> [--sub <id|gokey>] [--limit N]"
            " [--field filename|description|uploader]\n"
            "  missing [<sub-id|gokey>] [--update]");
        return 1;
    }

    /* file_argv[0] = subcommand name, file_argv[1..] = remaining args */
    file_argc = g_argc - 2;
    file_argv = g_argv + 2;

    for (fc = file_commands; fc->name; fc++) {
        if (strcmp(file_argv[0], fc->name) == 0)
            return fc->handler(myp, file_argc, file_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown file command: %s", file_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: news (news/gfile/pfile dispatch) ---------- */

struct news_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct news_command news_commands[] = {
    { "list",   cmd_news_list   },
    { "read",   cmd_news_read   },
    { "post",   cmd_news_post   },
    { "edit",   cmd_news_edit   },
    { "delete", cmd_news_delete },
    { NULL,     NULL            }
};

static int cmd_news(void)
{
    const struct news_command *nc;
    int news_argc;
    char **news_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli news <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list <sub-id|gokey> [--limit N] [--offset N]\n"
            "  read <sub-id|gokey> <item-number>\n"
            "  post <sub-id|gokey> --title <t> --author <acct>"
            " --text <t>\n"
            "  edit <sub-id|gokey> <item-number> --text <t>\n"
            "  delete <sub-id|gokey> <item-number>");
        return 1;
    }

    /* news_argv[0] = subcommand name, news_argv[1..] = remaining args */
    news_argc = g_argc - 2;
    news_argv = g_argv + 2;

    for (nc = news_commands; nc->name; nc++) {
        if (strcmp(news_argv[0], nc->name) == 0)
            return nc->handler(myp, news_argc, news_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown news command: %s", news_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: gfile (GFile dispatch) ---------- */

struct gfile_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct gfile_command gfile_commands[] = {
    { "list",   cmd_gfile_list   },
    { "read",   cmd_gfile_read   },
    { "add",    cmd_gfile_add    },
    { "remove", cmd_gfile_remove },
    { NULL,     NULL             }
};

static int cmd_gfile(void)
{
    const struct gfile_command *gfc;
    int gfile_argc;
    char **gfile_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli gfile <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list <sub-id|gokey> [--limit N] [--offset N]\n"
            "  read <sub-id|gokey> <item-number>\n"
            "  add <sub-id|gokey> --title <t> --author <acct>"
            " --text <t>\n"
            "  remove <sub-id|gokey> <item-number>");
        return 1;
    }

    /* gfile_argv[0] = subcommand name, gfile_argv[1..] = remaining args */
    gfile_argc = g_argc - 2;
    gfile_argv = g_argv + 2;

    for (gfc = gfile_commands; gfc->name; gfc++) {
        if (strcmp(gfile_argv[0], gfc->name) == 0)
            return gfc->handler(myp, gfile_argc, gfile_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown gfile command: %s", gfile_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: group (access group dispatch) ---------- */

struct group_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct group_command group_commands[] = {
    { "list",      cmd_group_list      },
    { "show",      cmd_group_show      },
    { "edit",      cmd_group_edit      },
    { "transpose", cmd_group_transpose },
    { NULL,        NULL                }
};

static int cmd_group(void)
{
    const struct group_command *gc;
    int group_argc;
    char **group_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli group <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list                   List all access groups\n"
            "  show <id>              Show group detail\n"
            "  edit <id> [--flags]    Edit group fields\n"
            "  transpose <id>         Push DefPrivs to all members");
        return 1;
    }

    group_argc = g_argc - 2;
    group_argv = g_argv + 2;

    for (gc = group_commands; gc->name; gc++) {
        if (strcmp(group_argv[0], gc->name) == 0)
            return gc->handler(myp, group_argc, group_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown group command: %s", group_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: config (config dispatch) ---------- */

struct config_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct config_command config_commands[] = {
    { "show",        cmd_config_show        },
    { "flags",       cmd_config_flags       },
    { "reload-text", cmd_config_reload_text },
    { "port",        cmd_config_port        },
    { NULL,          NULL                   }
};

static int cmd_config(void)
{
    const struct config_command *cc;
    int config_argc;
    char **config_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli config <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  show                   BBS configuration\n"
            "  flags [--set flag=val] Control panel flags\n"
            "  reload-text            Reload BBSTEXT/BBSMENU\n"
            "  port <port-number>     Per-port configuration");
        return 1;
    }

    config_argc = g_argc - 2;
    config_argv = g_argv + 2;

    for (cc = config_commands; cc->name; cc++) {
        if (strcmp(config_argv[0], cc->name) == 0)
            return cc->handler(myp, config_argc, config_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown config command: %s", config_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: log (log dispatch) ---------- */

struct log_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct log_command log_commands[] = {
    { "list",           cmd_log_list           },
    { "read",           cmd_log_read           },
    { "callers",        cmd_log_callers        },
    { "callers-parsed", cmd_log_callers_parsed },
    { NULL,             NULL                   }
};

static int cmd_log(void)
{
    const struct log_command *lc;
    int log_argc;
    char **log_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli log <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list              List log files\n"
            "  read <name> [--tail N] [--lines N]\n"
            "                    Read log file contents\n"
            "  callers [--tail N]\n"
            "                    Read callers log\n"
            "  callers-parsed [--tail N]\n"
            "                    Structured calls log parser");
        return 1;
    }

    log_argc = g_argc - 2;
    log_argv = g_argv + 2;

    for (lc = log_commands; lc->name; lc++) {
        if (strcmp(log_argv[0], lc->name) == 0)
            return lc->handler(myp, log_argc, log_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown log command: %s", log_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: stats ---------- */

static int cmd_stats(void)
{
    return cmd_stats_show(myp);
}

/* ---------- command: arexx (ARexx dispatch) ---------- */

struct arexx_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct arexx_command arexx_commands[] = {
    { "send",    cmd_arexx_send    },
    { "control", cmd_arexx_control },
    { NULL,      NULL              }
};

static int cmd_arexx(void)
{
    const struct arexx_command *ac;
    int arexx_argc;
    char **arexx_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli arexx <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  send <port-number> <command...>\n"
            "  control <command...>");
        return 1;
    }

    /* arexx_argv[0] = "send" or "control", [1..] = remaining args */
    arexx_argc = g_argc - 2;
    arexx_argv = g_argv + 2;

    for (ac = arexx_commands; ac->name; ac++) {
        if (strcmp(arexx_argv[0], ac->name) == 0)
            return ac->handler(myp, arexx_argc, arexx_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown arexx command: %s", arexx_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: port (port management dispatch) ---------- */

struct port_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct port_command port_commands[] = {
    { "load",   cmd_port_load   },
    { "unload", cmd_port_unload },
    { "dump",   cmd_port_dump   },
    { NULL,     NULL            }
};

static int cmd_port(void)
{
    const struct port_command *pc;
    int port_argc;
    char **port_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli port <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  load <port-number>\n"
            "  unload <port-number>\n"
            "  dump <port-number>");
        return 1;
    }

    /* port_argv[0] = "load"/"unload"/"dump", [1..] = remaining args */
    port_argc = g_argc - 2;
    port_argv = g_argv + 2;

    for (pc = port_commands; pc->name; pc++) {
        if (strcmp(port_argv[0], pc->name) == 0)
            return pc->handler(myp, port_argc, port_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown port command: %s", port_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: olm ---------- */

static int cmd_olm(void)
{
    int olm_argc = g_argc - 2;
    char **olm_argv = g_argv + 2;

    if (g_argc < 7) {
        json_error(
            "Usage: cnet-cli olm <port>"
            " --from <account> --text \"message\""
            " [--broadcast]");
        return 1;
    }

    return cmd_olm_send(myp, olm_argc, olm_argv);
}

/* ---------- command: conf (conference room dispatch) ---------- */

struct conf_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct conf_command conf_commands[] = {
    { "list", cmd_conf_list },
    { NULL,   NULL          }
};

static int cmd_conf(void)
{
    const struct conf_command *cc;
    int conf_argc;
    char **conf_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli conf <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list [--all]          List conference rooms");
        return 1;
    }

    conf_argc = g_argc - 2;
    conf_argv = g_argv + 2;

    for (cc = conf_commands; cc->name; cc++) {
        if (strcmp(conf_argv[0], cc->name) == 0)
            return cc->handler(myp, conf_argc, conf_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown conf command: %s", conf_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: event (event dispatch) ---------- */

struct event_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct event_command event_commands[] = {
    { "list", cmd_event_list },
    { "show", cmd_event_show },
    { NULL,   NULL           }
};

static int cmd_event(void)
{
    const struct event_command *ec;
    int event_argc;
    char **event_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli event <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list [--all]          List scheduled events\n"
            "  show <index>          Show event detail");
        return 1;
    }

    event_argc = g_argc - 2;
    event_argv = g_argv + 2;

    for (ec = event_commands; ec->name; ec++) {
        if (strcmp(event_argv[0], ec->name) == 0)
            return ec->handler(myp, event_argc, event_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown event command: %s", event_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: maint (maintenance dispatch) ---------- */

struct maint_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct maint_command maint_commands[] = {
    { "pointers",    cmd_maint_pointers    },
    { "count",       cmd_maint_count       },
    { "repair-mail", cmd_maint_repair_mail },
    { "repair-sub",  cmd_maint_repair_sub  },
    { NULL,          NULL                  }
};

static int cmd_maint(void)
{
    const struct maint_command *mc;
    int maint_argc;
    char **maint_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli maint <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  pointers               Rebuild user index files\n"
            "  count [--apply] [--sub <id|gokey>] [--subs-only] [--nums-only]\n"
            "                         Recount subboard/system counters\n"
            "  repair-mail [<acct|handle>] [--folder <name>] --apply\n"
            "                         Compact mail data files\n"
            "  repair-sub <id|gokey> --apply\n"
            "                         Compact subboard text pool");
        return 1;
    }

    maint_argc = g_argc - 2;
    maint_argv = g_argv + 2;

    for (mc = maint_commands; mc->name; mc++) {
        if (strcmp(maint_argv[0], mc->name) == 0)
            return mc->handler(myp, maint_argc, maint_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown maint command: %s", maint_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: bbslist (BBSList dispatch) ---------- */

struct bbslist_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct bbslist_command bbslist_commands[] = {
    { "list", cmd_bbslist_list },
    { NULL,   NULL             }
};

static int cmd_bbslist(void)
{
    const struct bbslist_command *bc;
    int bbslist_argc;
    char **bbslist_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli bbslist <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list [--all]          List BBS entries");
        return 1;
    }

    bbslist_argc = g_argc - 2;
    bbslist_argv = g_argv + 2;

    for (bc = bbslist_commands; bc->name; bc++) {
        if (strcmp(bbslist_argv[0], bc->name) == 0)
            return bc->handler(myp, bbslist_argc, bbslist_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown bbslist command: %s", bbslist_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command: vote (voting booth dispatch) ---------- */

struct vote_command {
    const char *name;
    int (*handler)(struct MainPort *, int, char **);
};

static const struct vote_command vote_commands[] = {
    { "list",    cmd_vote_list    },
    { "show",    cmd_vote_show    },
    { "results", cmd_vote_results },
    { NULL,      NULL             }
};

static int cmd_vote(void)
{
    const struct vote_command *vc;
    int vote_argc;
    char **vote_argv;

    if (g_argc < 3) {
        json_error(
            "Usage: cnet-cli vote <subcommand> [args]\n"
            "\n"
            "Subcommands:\n"
            "  list                  List vote topics\n"
            "  show <number>         Show topic detail with choices/results\n"
            "  results <number>      Show vote results only");
        return 1;
    }

    vote_argc = g_argc - 2;
    vote_argv = g_argv + 2;

    for (vc = vote_commands; vc->name; vc++) {
        if (strcmp(vote_argv[0], vc->name) == 0)
            return vc->handler(myp, vote_argc, vote_argv);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Unknown vote command: %s", vote_argv[0]);
        json_error(buf);
    }
    return 1;
}

/* ---------- command dispatch ---------- */

struct command {
    const char *name;
    int (*handler)(void);
};

static const struct command commands[] = {
    { "status", cmd_status },
    { "ports",  cmd_ports  },
    { "who",    cmd_who    },
    { "sub",    cmd_sub    },
    { "msg",    cmd_msg    },
    { "user",   cmd_user   },
    { "file",   cmd_file   },
    { "news",   cmd_news   },
    { "gfile",  cmd_gfile  },
    { "olm",    cmd_olm    },
    { "mail",   cmd_mail   },
    { "group",  cmd_group  },
    { "config", cmd_config },
    { "log",    cmd_log    },
    { "stats",  cmd_stats  },
    { "arexx",  cmd_arexx  },
    { "port",   cmd_port   },
    { "conf",   cmd_conf   },
    { "event",   cmd_event   },
    { "maint",   cmd_maint   },
    { "bbslist", cmd_bbslist },
    { "vote",    cmd_vote    },
    { NULL,      NULL        }
};

static void print_usage(void)
{
    json_error(
        "Usage: cnet-cli <command> [subcommand] [args]\n"
        "\n"
        "Commands:\n"
        "  status                System overview\n"
        "  ports                 All port status\n"
        "  who                   Online users\n"
        "  who --detail          Extended online user info\n"
        "  who <port>            Specific port detail\n"
        "  sub list [--active] [--type msg|file|door]\n"
        "                        List subboards\n"
        "  sub show <id|gokey>   Subboard detail\n"
        "  sub tree              Subboard tree\n"
        "  sub path <id|gokey>   Subboard ancestry path\n"
        "  sub create ...        Create a subboard\n"
        "  sub edit <id> ...     Edit subboard fields\n"
        "  sub delete <id>       Delete (kill) a subboard\n"
        "  msg list <sub> [--limit N] [--offset N]\n"
        "                        List messages in a subboard\n"
        "  msg read <sub> <num>  Read message with text + responses\n"
        "  msg post <sub> ...    Post a new message\n"
        "  msg respond <sub> <num> ...\n"
        "                        Respond to a message\n"
        "  msg delete <sub> <num>\n"
        "                        Delete (kill) a message\n"
        "  file list <sub> [--limit N] [--offset N]\n"
        "                        List files in a file area\n"
        "  file show <sub> <num> Show file entry details\n"
        "  file add <sub> --title F --author A [--desc D]\n"
        "                        Register a file in the catalog\n"
        "  file edit <sub> <num> [--validated N] [--frozen N] ...\n"
        "                        Edit file entry attributes\n"
        "  file remove <sub> <num> [--delete-physical]\n"
        "                        Remove file entry (kill)\n"
        "  file validate <sub> <range>\n"
        "                        Validate file entries\n"
        "  file find <query> [--sub <id>] [--limit N]\n"
        "    [--field filename|description|uploader]\n"
        "                        Search files across subboards\n"
        "  file missing [<sub>] [--update]\n"
        "                        Detect missing/restored files\n"
        "  news list <sub> [--limit N] [--offset N]\n"
        "                        List items in a text/door area\n"
        "  news read <sub> <num> Read news item with text\n"
        "  news post <sub> --title T --author A --text T\n"
        "                        Post a new news item\n"
        "  news edit <sub> <num> --text T\n"
        "                        Edit news item text\n"
        "  news delete <sub> <num>\n"
        "                        Delete (kill) a news item\n"
        "  gfile list <sub> [--limit N] [--offset N]\n"
        "                        List items in a GFile area\n"
        "  gfile read <sub> <num>\n"
        "                        Read GFile item with text\n"
        "  gfile add <sub> --title T --author A --text T\n"
        "                        Add a new GFile item\n"
        "  gfile remove <sub> <num>\n"
        "                        Remove (kill) a GFile item\n"
        "  user list [--group N] List all user accounts\n"
        "  user show <acct|handle>\n"
        "                        User detail\n"
        "  user find <query> [--phone]\n"
        "                        Search users by handle/name or phone\n"
        "  user plan <acct|handle>\n"
        "                        User plan file\n"
        "  user edit <acct> ...  Edit user fields\n"
        "  user disable <acct>   Suspend account\n"
        "  user enable <acct>    Re-enable account\n"
        "  user profile <acct|handle>\n"
        "                        Public user profile\n"
        "  user delete <acct|handle> --force\n"
        "                        Delete user account\n"
        "  olm <acct> --text T [--from N]\n"
        "                        Send Online Message\n"
        "  mail send --from <u> --to <u> --subject <s> --body <t>\n"
        "                        Send mail\n"
        "  mail list <acct> [--folder <name>] [--limit N] [--offset N]\n"
        "                        List mail in folder\n"
        "  mail read <acct> <num> [--folder <name>]\n"
        "                        Read a mail message\n"
        "  mail reply <acct> <num> --body <t>\n"
        "                        Reply to a mail message\n"
        "  mail delete <acct> <num> [--folder <name>]\n"
        "                        Delete a mail message\n"
        "  mail folders <acct>   List mail folders\n"
        "  mail count <acct> [--folder <name>]\n"
        "                        Count messages in folder\n"
        "  mail feedback [<num>] [--folder <name>]\n"
        "                        Sysop feedback mail\n"
        "  mail verify <acct|handle>\n"
        "                        View sent mail\n"
        "  mail alias list <acct|handle>\n"
        "                        List mail aliases\n"
        "  mail alias add <acct|handle> --alias <name>\n"
        "    --name <recipient> [--address <addr>]\n"
        "                        Add a mail alias\n"
        "  mail alias remove <acct|handle> --alias <name>\n"
        "    [--name <recipient>]\n"
        "                        Remove a mail alias\n"
        "  group list            List access groups\n"
        "  group show <id>       Access group detail\n"
        "  config show           BBS configuration\n"
        "  config flags [--set flag=val ...]\n"
        "                        Control panel flags\n"
        "  config reload-text    Reload BBSTEXT/BBSMENU\n"
        "  config port <N>       Per-port configuration\n"
        "  stats                 System statistics\n"
        "  log list              List log files\n"
        "  log read <name> [--tail N] [--lines N]\n"
        "                        Read log file contents\n"
        "  log callers [--tail N]\n"
        "                        Read callers log\n"
        "  log callers-parsed [--tail N]\n"
        "                        Structured calls log parser\n"
        "  arexx send <port> <cmd...>\n"
        "                        Send ARexx command to CNETREXX{N}\n"
        "  arexx control <cmd...>\n"
        "                        Send ARexx command to CONTROLREXX.1\n"
        "  port load <port>      Load a BBS port\n"
        "  port unload <port>    Unload a BBS port\n"
        "  port dump <port>      Disconnect user on a port\n"
        "  conf list [--all]     List conference rooms\n"
        "  event list [--all]    List scheduled events\n"
        "  event show <index>    Show event detail\n"
        "  maint pointers        Rebuild user index files\n"
        "  maint count [--apply] [--sub <id>] [--subs-only] [--nums-only]\n"
        "                         Recount subboard/system counters\n"
        "  maint repair-mail [<acct>] [--folder <name>] --apply\n"
        "                         Compact mail data files\n"
        "  maint repair-sub <id|gokey> --apply\n"
        "                         Compact subboard text pool\n"
        "  bbslist list [--all]  List BBS directory entries\n"
        "  vote list             List vote topics\n"
        "  vote show <number>    Vote topic detail with choices/results\n"
        "  vote results <number> Vote results only");
}

/* ---------- init / cleanup ---------- */

static int init_cnet(void)
{
    CNetBase = (struct Library *)OpenLibrary((CONST_STRPTR)"cnet.library", 0);
    if (!CNetBase) {
        json_error("Cannot open cnet.library (is CNet running?)");
        return 1;
    }

    Forbid();
    myp = (struct MainPort *)FindPort((CONST_STRPTR)"cnetport");
    Permit();

    if (!myp) {
        json_error("MainPort not found (is CNet running?)");
        CloseLibrary(CNetBase);
        CNetBase = NULL;
        return 1;
    }

    /*
     * Open cnetmail.library for mail send/reply (SendMail) and
     * mail semaphores (GetMailSems). Non-fatal if missing -- mail
     * commands check CNetMailBase before use, and non-mail commands
     * work normally regardless.
     */
    CNetMailBase = (struct Library *)OpenLibrary(
        (CONST_STRPTR)"cnetmail.library", 0);
    if (!CNetMailBase) {
        warn_add("Cannot open cnetmail.library"
            " (mail send/reply unavailable)");
    }

    /*
     * Open rexxsyslib.library for ARexx IPC (arexx send/control, port
     * load/unload/dump). Non-fatal if missing -- arexx/port commands
     * check RexxSysBase before use, and other commands work normally.
     */
    RexxSysBase = (struct RxsLib *)OpenLibrary(
        (CONST_STRPTR)"rexxsyslib.library", 0);
    if (!RexxSysBase) {
        warn_add("Cannot open rexxsyslib.library"
            " (arexx/port commands unavailable)");
    }

    /*
     * Open cnet4.library for timestamp functions (CNetTime,
     * CNetExplodeTime, CNetImplodeTime) and range parsing
     * (CNetFindRange, CNetNextRange). Non-fatal if missing --
     * OLM timestamp code falls back to DateStamp() if unavailable.
     */
    CNet4Base = (struct Library *)OpenLibrary(
        (CONST_STRPTR)"cnet4.library", 0);
    if (!CNet4Base) {
        warn_add("Cannot open cnet4.library"
            " (timestamp/range functions unavailable)");
    }

    return 0;
}

static void cleanup_cnet(void)
{
    if (CNet4Base) {
        CloseLibrary(CNet4Base);
        CNet4Base = NULL;
    }
    if (RexxSysBase) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
    }
    if (CNetMailBase) {
        CloseLibrary(CNetMailBase);
        CNetMailBase = NULL;
    }
    if (CNetBase) {
        CloseLibrary(CNetBase);
        CNetBase = NULL;
    }
    myp = NULL;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    const struct command *cmd;
    int rc;

    warn_clear();

    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Find the requested command */
    for (cmd = commands; cmd->name; cmd++) {
        if (strcmp(argv[1], cmd->name) == 0)
            break;
    }

    if (!cmd->name) {
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Unknown command: %s", argv[1]);
            json_error(buf);
        }
        return 1;
    }

    /* Save for two-level dispatch (cmd_sub, etc.) */
    g_argc = argc;
    g_argv = argv;

    /* Initialize CNet access */
    rc = init_cnet();
    if (rc != 0)
        return rc;

    /* Dispatch */
    rc = cmd->handler();

    /* Cleanup */
    cleanup_cnet();
    return rc;
}
