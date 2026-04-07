/*
 * user.c -- User management commands for cnet-cli
 *
 * User operations: list, show, find, edit, disable, enable, delete, profile; who detail; OLM
 *
 * User account access uses LockAccount/UnLockAccount from cnet.library.
 * Every successful LockAccount MUST be paired with UnLockAccount in all
 * code paths (success and error).
 *
 * Key[] array reads use SEM[1] shared lock.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <exec/types.h>
#include <dos/dos.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/cnet4.h>
#include "user.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/* From main.c -- may be NULL if cnet4.library is not available */
extern struct Library *CNet4Base;

/* ---- internal helpers ---- */

/*
 * Resolve a user identifier to an account number.
 * Thin wrapper around resolve_user_full() in util.c.
 *
 * Account numbers are 1-based. Returns account number (>= 1)
 * or -1 if not found.
 */
static short resolve_user(struct MainPort *myp, const char *id_or_handle)
{
    return resolve_user_full(myp, id_or_handle, NULL, 0);
}

/*
 * Emit summary fields for one user from the Key[] array.
 * Used by user list and user find.
 */
static void emit_user_summary(struct json_state *js,
    struct KeyElement4 *key, int account_num,
    struct MainPort *myp)
{
    char buf[128];

    json_obj_open(js);
    json_kv_int(js, "account", (long)account_num);
    json_kv_str(js, "handle",
        strip_mci(buf, sizeof(buf), key->Handle));

    /* Respect privacy flag: only show real name if public */
    if (key->PName == 0) {
        json_kv_str(js, "real_name",
            strip_mci(buf, sizeof(buf), key->RealName));
    } else {
        json_kv_null(js, "real_name");
    }

    json_kv_int(js, "access_group", (long)key->Access);

    /* Resolve access group name */
    if (key->Access >= 0 && key->Access < 32) {
        json_kv_str(js, "group_name",
            strip_mci(buf, sizeof(buf),
                myp->AGC[(int)key->Access].Name));
    } else {
        json_kv_null(js, "group_name");
    }

    json_kv_str(js, "uucp", key->UUCP);
    json_kv_int(js, "id_number", key->IDNumber);
    json_obj_close(js);
}

/*
 * Emit all UserData fields as JSON for user show.
 * The JSON object must already be open.
 * Does NOT open/close the object (caller does that).
 *
 * Password is NEVER emitted (security).
 */
static void emit_user_detail(struct json_state *js,
    struct UserData *user, int account,
    struct MainPort *myp)
{
    char buf[128];
    char dbuf[24];
    char abuf[16];

    /* Identity fields */
    json_kv_int(js, "account", (long)account);
    json_kv_int(js, "id_number", user->IDNumber);
    json_kv_str(js, "handle",
        strip_mci(buf, sizeof(buf), user->Handle));
    json_kv_str(js, "real_name",
        strip_mci(buf, sizeof(buf), user->RealName));
    json_kv_str(js, "uucp", user->UUCP);

    /* Address type classification */
    {
        int atype = address_type(user->UUCP);
        const char *atype_name;
        switch (atype) {
        case 1:  atype_name = "local"; break;
        case 2:  atype_name = "internet"; break;
        default: atype_name = "unknown"; break;
        }
        json_kv_str(js, "address_type", atype_name);
    }

    /* Contact/personal fields */
    json_kv_str(js, "address", user->Address);
    json_kv_str(js, "city_state", user->CityState);
    json_kv_str(js, "zip_code", user->ZipCode);
    json_kv_str(js, "country", user->Country);
    json_kv_str(js, "phone_data", user->PhoneNo);
    json_kv_str(js, "phone_voice", user->VoiceNo);
    json_kv_str(js, "organization", user->Organ);
    json_kv_str(js, "comments", user->Comments);
    json_kv_str(js, "banner",
        strip_mci(buf, sizeof(buf), user->Banner));

    /* Access/status */
    json_kv_int(js, "access_group", (long)user->Access);
    if (user->Access >= 0 && user->Access < 32) {
        json_kv_str(js, "group_name",
            strip_mci(buf, sizeof(buf),
                myp->AGC[(int)user->Access].Name));
    } else {
        json_kv_null(js, "group_name");
    }
    json_kv_int(js, "expire_access", (long)user->ExpireAccess);
    json_kv_bool(js, "suspended",
        (user->MyPrivs.ABits & SUSPENDACCT_FLAG) ? 1 : 0);
    json_kv_int(js, "phone_verified", (long)user->PhoneVerified);

    /* Dates */
    if (is_null_date(&user->Birthdate))
        json_kv_null(js, "birthdate");
    else
        json_kv_str(js, "birthdate",
            format_date(dbuf, sizeof(dbuf), &user->Birthdate));

    if (is_null_date(&user->FirstCall))
        json_kv_null(js, "first_call");
    else
        json_kv_str(js, "first_call",
            format_date(dbuf, sizeof(dbuf), &user->FirstCall));

    if (is_null_date(&user->LastCall))
        json_kv_null(js, "last_call");
    else
        json_kv_str(js, "last_call",
            format_date(dbuf, sizeof(dbuf), &user->LastCall));

    if (is_null_date(&user->ExpireDate))
        json_kv_null(js, "expire_date");
    else
        json_kv_str(js, "expire_date",
            format_date(dbuf, sizeof(dbuf), &user->ExpireDate));

    /* Statistics */
    json_kv_int(js, "total_calls", user->TotalCalls);
    json_kv_int(js, "pub_messages", user->PubMessages);
    json_kv_int(js, "pri_messages", user->PriMessages);
    json_kv_int(js, "up_kbytes", user->UpBytes);
    json_kv_int(js, "up_files", user->UpFiles);
    json_kv_int(js, "down_kbytes", user->DownBytes);
    json_kv_int(js, "down_files", user->DownFiles);
    json_kv_int(js, "file_credits", user->FileCredits);
    json_kv_int(js, "byte_credits", user->ByteCredits);
    json_kv_int(js, "time_credits", user->TimeCredits);
    json_kv_int(js, "balance", user->Balance);
    json_kv_int(js, "door_points", user->DoorPoints);

    /* Terminal settings */
    json_kv_int(js, "term_width", (long)user->TermWidth);
    json_kv_int(js, "term_length", (long)user->TermLength);
    json_kv_int(js, "colors", (long)user->Colors);
    json_kv_int(js, "ansi", (long)user->ANSI);

    /* Privilege flags as hex strings */
    snprintf(abuf, sizeof(abuf), "0x%08lx",
        (unsigned long)user->MyPrivs.ABits);
    json_kv_str(js, "abits", abuf);
    snprintf(abuf, sizeof(abuf), "0x%08lx",
        (unsigned long)user->MyPrivs.ABits2);
    json_kv_str(js, "abits2", abuf);

    /* Privilege limits (commonly checked subset) */
    json_kv_int(js, "daily_minutes",
        (long)user->MyPrivs.DailyMinutes);
    json_kv_int(js, "idle_limit", (long)user->MyPrivs.Idle);
    json_kv_int(js, "editor_lines",
        (long)user->MyPrivs.EditorLines);
}

/* ---- user list ---- */

int cmd_user_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int i;
    int group_filter = -1;  /* -1 = no filter */
    int limit = -1;         /* -1 = no limit */
    int offset = 0;
    int skipped = 0;
    int emitted = 0;
    long num_accounts;

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--group") == 0) {
            if (i + 1 >= argc) {
                json_error("--group requires an argument");
                return 1;
            }
            i++;
            group_filter = atoi(argv[i]);
            if (group_filter < 0 || group_filter > 31) {
                json_error("--group must be 0-31");
                return 1;
            }
        } else if (strcmp(argv[i], "--limit") == 0) {
            if (i + 1 >= argc) {
                json_error("--limit requires an argument");
                return 1;
            }
            i++;
            limit = atoi(argv[i]);
        } else if (strcmp(argv[i], "--offset") == 0) {
            if (i + 1 >= argc) {
                json_error("--offset requires an argument");
                return 1;
            }
            i++;
            offset = atoi(argv[i]);
        }
    }

    ObtainSemaphoreShared(&myp->SEM[1]);

    num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "users");
    json_arr_open(&js);

    for (i = 0; i < (int)num_accounts; i++) {
        struct KeyElement4 *key = &myp->Key[i];

        /* Skip empty entries */
        if (key->Handle[0] == '\0')
            continue;

        /* Apply group filter */
        if (group_filter >= 0 && (int)key->Access != group_filter)
            continue;

        /* Apply offset */
        if (skipped < offset) {
            skipped++;
            continue;
        }

        /* Apply limit */
        if (limit >= 0 && emitted >= limit)
            break;

        emit_user_summary(&js, key, i + 1, myp);
        emitted++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "total_slots", num_accounts);
    json_kv_int(&js, "matched", (long)emitted);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[1]);
    return 0;
}

/* ---- user show ---- */

int cmd_user_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;

    if (argc < 2) {
        json_error("Usage: cnet-cli user show <account|handle>");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    emit_user_detail(&js, user, (int)account, myp);
    json_obj_close(&js);
    json_finish(&js);

    UnLockAccount(account, 0);
    return 0;
}

/* ---- user find ---- */

int cmd_user_find(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int phone_mode = 0;
    const char *query = NULL;
    int i;
    int emitted = 0;
    long num_accounts;

    /* Parse flags and find query argument */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--phone") == 0) {
            phone_mode = 1;
        } else if (!query) {
            query = argv[i];
        }
    }

    if (!query) {
        json_error("Usage: cnet-cli user find <query> [--phone]");
        return 1;
    }

    ObtainSemaphoreShared(&myp->SEM[1]);

    num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "users");
    json_arr_open(&js);

    if (phone_mode) {
        /*
         * FindPhone is a single-shot search, not an iterator.
         * It writes the sorted index into *n (into IPhone[]),
         * like FindHandle writes into IName[].  Call it once;
         * if it matches, map through IPhone[] to get the account.
         */
        short n = 0;
        if (FindPhone(&n, (char *)query, (short)0)) {
            short account = myp->IPhone[n];
            if (account >= 1 && account <= (short)num_accounts) {
                struct KeyElement4 *key = &myp->Key[account - 1];
                emit_user_summary(&js, key, (int)account, myp);
                emitted++;
            }
        }
    } else {
        for (i = 0; i < (int)num_accounts; i++) {
            struct KeyElement4 *key = &myp->Key[i];

            /* Skip empty entries */
            if (key->Handle[0] == '\0')
                continue;

            /* Case-insensitive substring match on Handle or RealName */
            if (ci_contains(key->Handle, query) ||
                ci_contains(key->RealName, query)) {
                emit_user_summary(&js, key, i + 1, myp);
                emitted++;
            }
        }
    }

    json_arr_close(&js);
    json_kv_int(&js, "matched", (long)emitted);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[1]);
    return 0;
}

/* ---- user plan ---- */

#define PLAN_MAX_SIZE 4096

int cmd_user_plan(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp_buf[12];
    char path[128];
    char buf[128];
    BPTR fh;

    if (argc < 2) {
        json_error("Usage: cnet-cli user plan <account|handle>");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    /* Read UUCP name from Key[] under SEM[1] shared */
    ObtainSemaphoreShared(&myp->SEM[1]);

    if (myp->Key[account - 1].UUCP[0] == '\0') {
        ReleaseSemaphore(&myp->SEM[1]);
        json_error("No UUCP name for this account");
        return 1;
    }

    safe_strcpy(uucp_buf, myp->Key[account - 1].UUCP, sizeof(uucp_buf));

    /* Also grab handle while we have the semaphore */
    strip_mci(buf, sizeof(buf), myp->Key[account - 1].Handle);

    ReleaseSemaphore(&myp->SEM[1]);

    /* Build path to plan file */
    snprintf(path, sizeof(path), "mail:users/%s/_plan", uucp_buf);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "handle", buf);
    json_kv_str(&js, "uucp", uucp_buf);

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        json_kv_null(&js, "plan");
    } else {
        long size;
        long bytes_read;
        static char plan_buf[PLAN_MAX_SIZE + 1];

        /* Get file size */
        Seek(fh, 0, OFFSET_END);
        size = Seek(fh, 0, OFFSET_BEGINNING);
        if (size > PLAN_MAX_SIZE)
            size = PLAN_MAX_SIZE;
        if (size < 0)
            size = 0;

        bytes_read = Read(fh, plan_buf, size);
        Close(fh);

        if (bytes_read < 0)
            bytes_read = 0;
        plan_buf[bytes_read] = '\0';

        json_kv_str(&js, "plan", plan_buf);
    }

    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---- user edit ---- */

int cmd_user_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;
    int rc = 0;
    UBYTE save = 0;
    int i;
    int nchanged = 0;

    /* Field change values (NULL = don't change) */
    const char *new_handle = NULL;
    const char *new_realname = NULL;
    const char *new_comment = NULL;
    const char *new_address = NULL;
    const char *new_city = NULL;
    const char *new_country = NULL;
    const char *new_phone_data = NULL;
    const char *new_phone_voice = NULL;
    const char *new_organization = NULL;
    const char *new_banner = NULL;
    int new_access = -1;  /* -1 = don't change */

    /* Track which fields were changed for output */
    const char *changed_fields[16];
    int changed_count = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli user edit <account|handle> "
            "[--handle H] [--realname N] ...");
        return 1;
    }

    /* Parse --field value pairs */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--handle") == 0 && i + 1 < argc) {
            new_handle = argv[++i];
        } else if (strcmp(argv[i], "--realname") == 0 &&
                   i + 1 < argc) {
            new_realname = argv[++i];
        } else if (strcmp(argv[i], "--comment") == 0 &&
                   i + 1 < argc) {
            new_comment = argv[++i];
        } else if (strcmp(argv[i], "--address") == 0 &&
                   i + 1 < argc) {
            new_address = argv[++i];
        } else if (strcmp(argv[i], "--city") == 0 && i + 1 < argc) {
            new_city = argv[++i];
        } else if (strcmp(argv[i], "--country") == 0 &&
                   i + 1 < argc) {
            new_country = argv[++i];
        } else if (strcmp(argv[i], "--phone-data") == 0 &&
                   i + 1 < argc) {
            new_phone_data = argv[++i];
        } else if (strcmp(argv[i], "--phone-voice") == 0 &&
                   i + 1 < argc) {
            new_phone_voice = argv[++i];
        } else if (strcmp(argv[i], "--organization") == 0 &&
                   i + 1 < argc) {
            new_organization = argv[++i];
        } else if (strcmp(argv[i], "--banner") == 0 &&
                   i + 1 < argc) {
            new_banner = argv[++i];
        } else if (strcmp(argv[i], "--access") == 0 &&
                   i + 1 < argc) {
            i++;
            new_access = atoi(argv[i]);
            if (new_access < 0 || new_access > 31) {
                json_error("--access must be 0-31");
                return 1;
            }
        } else if (strncmp(argv[i], "--", 2) == 0) {
            json_error("Unknown flag");
            return 1;
        }
    }

    /* Check that at least one field was specified */
    nchanged = (new_handle != NULL) + (new_realname != NULL) +
        (new_comment != NULL) + (new_address != NULL) +
        (new_city != NULL) + (new_country != NULL) +
        (new_phone_data != NULL) + (new_phone_voice != NULL) +
        (new_organization != NULL) + (new_banner != NULL) +
        (new_access >= 0);

    if (nchanged == 0) {
        json_error("No fields to edit");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    /* Check if user is online -- warn but proceed */
    {
        struct PortData *online_z = IsNowOnLine(myp, account);
        if (online_z) {
            {
                char wbuf[128];
                snprintf(wbuf, sizeof(wbuf),
                    "User is online on port %d"
                    " -- changes may be overwritten at logoff",
                    (int)online_z->InPort);
                warn_add(wbuf);
            }
        }
    }

    /* Apply field changes */
    if (new_handle) {
        safe_strcpy(user->Handle, new_handle,
            (int)sizeof(user->Handle));
        if (changed_count < 16)
            changed_fields[changed_count++] = "handle";
    }
    if (new_realname) {
        safe_strcpy(user->RealName, new_realname,
            (int)sizeof(user->RealName));
        if (changed_count < 16)
            changed_fields[changed_count++] = "real_name";
    }
    if (new_comment) {
        safe_strcpy(user->Comments, new_comment,
            (int)sizeof(user->Comments));
        if (changed_count < 16)
            changed_fields[changed_count++] = "comments";
    }
    if (new_address) {
        safe_strcpy(user->Address, new_address,
            (int)sizeof(user->Address));
        if (changed_count < 16)
            changed_fields[changed_count++] = "address";
    }
    if (new_city) {
        safe_strcpy(user->CityState, new_city,
            (int)sizeof(user->CityState));
        if (changed_count < 16)
            changed_fields[changed_count++] = "city_state";
    }
    if (new_country) {
        safe_strcpy(user->Country, new_country,
            (int)sizeof(user->Country));
        if (changed_count < 16)
            changed_fields[changed_count++] = "country";
    }
    if (new_phone_data) {
        safe_strcpy(user->PhoneNo, new_phone_data,
            (int)sizeof(user->PhoneNo));
        if (changed_count < 16)
            changed_fields[changed_count++] = "phone_data";
    }
    if (new_phone_voice) {
        safe_strcpy(user->VoiceNo, new_phone_voice,
            (int)sizeof(user->VoiceNo));
        if (changed_count < 16)
            changed_fields[changed_count++] = "phone_voice";
    }
    if (new_organization) {
        safe_strcpy(user->Organ, new_organization,
            (int)sizeof(user->Organ));
        if (changed_count < 16)
            changed_fields[changed_count++] = "organization";
    }
    if (new_banner) {
        safe_strcpy(user->Banner, new_banner,
            (int)sizeof(user->Banner));
        if (changed_count < 16)
            changed_fields[changed_count++] = "banner";
    }
    if (new_access >= 0) {
        user->Access = (BYTE)new_access;
        if (changed_count < 16)
            changed_fields[changed_count++] = "access_group";
    }

    save = 1;

    UnLockAccount(account, save);

    /* Emit success JSON */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "updated");
    json_kv_int(&js, "account", (long)account);

    json_key(&js, "fields_changed");
    json_arr_open(&js);
    for (i = 0; i < changed_count; i++)
        json_str(&js, changed_fields[i]);
    json_arr_close(&js);

    if (new_handle) {
        json_kv_str(&js, "note",
            "Handle change requires CNet restart"
            " to update user list cache");
    }

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return rc;
}

/* ---- user disable ---- */

int cmd_user_disable(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;
    int already;
    char buf[128];

    if (argc < 2) {
        json_error("Usage: cnet-cli user disable <account|handle>");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    already = (user->MyPrivs.ABits & SUSPENDACCT_FLAG) ? 1 : 0;
    user->MyPrivs.ABits |= SUSPENDACCT_FLAG;

    /* Save the handle before unlocking */
    strip_mci(buf, sizeof(buf), user->Handle);

    UnLockAccount(account, 1);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "disabled");
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "handle", buf);
    json_kv_bool(&js, "was_already_disabled", already);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- user enable ---- */

int cmd_user_enable(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;
    int was_suspended;
    char buf[128];

    if (argc < 2) {
        json_error("Usage: cnet-cli user enable <account|handle>");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    was_suspended = (user->MyPrivs.ABits & SUSPENDACCT_FLAG) ? 1 : 0;
    user->MyPrivs.ABits &= ~SUSPENDACCT_FLAG;

    /* Save the handle before unlocking */
    strip_mci(buf, sizeof(buf), user->Handle);

    UnLockAccount(account, 1);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "enabled");
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "handle", buf);
    json_kv_bool(&js, "was_already_enabled", was_suspended ? 0 : 1);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- user profile ---- */

/*
 * Public user profile -- subset of user show with only public-facing
 * fields. Uses the same LockAccount/UnLockAccount pattern.
 *
 * PName privacy flag is read from UserData directly (offset 489 per
 * cnet/users.h), NOT from Key[] -- no SEM[1] needed since the account
 * is already locked.
 */
int cmd_user_profile(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;
    char buf[128];
    char dbuf[24];

    if (argc < 2) {
        json_error("Usage: cnet-cli user profile <account|handle>");
        return 1;
    }

    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    json_init(&js, stdout);
    json_obj_open(&js);

    /* Identity */
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "handle",
        strip_mci(buf, sizeof(buf), user->Handle));

    /* Real name: respect PName privacy flag from UserData */
    if (user->PName == 0) {
        json_kv_str(&js, "real_name",
            strip_mci(buf, sizeof(buf), user->RealName));
    } else {
        json_kv_null(&js, "real_name");
    }

    /* Access group */
    json_kv_int(&js, "access_group", (long)user->Access);
    if (user->Access >= 0 && user->Access < 32) {
        json_kv_str(&js, "group_name",
            strip_mci(buf, sizeof(buf),
                myp->AGC[(int)user->Access].Name));
    } else {
        json_kv_null(&js, "group_name");
    }

    /* Public info */
    json_kv_str(&js, "organization", user->Organ);
    json_kv_str(&js, "banner",
        strip_mci(buf, sizeof(buf), user->Banner));

    /* Dates */
    if (is_null_date(&user->LastCall))
        json_kv_null(&js, "last_call");
    else
        json_kv_str(&js, "last_call",
            format_date(dbuf, sizeof(dbuf), &user->LastCall));

    if (is_null_date(&user->FirstCall))
        json_kv_null(&js, "first_call");
    else
        json_kv_str(&js, "first_call",
            format_date(dbuf, sizeof(dbuf), &user->FirstCall));

    /* Statistics */
    json_kv_int(&js, "total_calls", user->TotalCalls);
    json_kv_int(&js, "pub_messages", user->PubMessages);
    json_kv_int(&js, "up_files", user->UpFiles);
    json_kv_int(&js, "up_kbytes", user->UpBytes);
    json_kv_int(&js, "down_files", user->DownFiles);
    json_kv_int(&js, "down_kbytes", user->DownBytes);

    /* Status */
    json_kv_bool(&js, "suspended",
        (user->MyPrivs.ABits & SUSPENDACCT_FLAG) ? 1 : 0);

    json_obj_close(&js);
    json_finish(&js);

    UnLockAccount(account, 0);
    return 0;
}

/* ---- who detail ---- */

/*
 * Emit extended detail for a single online port.
 */
static void emit_port_detail(struct json_state *js,
    struct PortData *z, struct MainPort *myp)
{
    char buf[128];

    json_obj_open(js);

    json_kv_int(js, "port", (long)z->InPort);
    json_kv_int(js, "account", (long)z->id);
    json_kv_str(js, "handle",
        strip_mci(buf, sizeof(buf), z->user1.Handle));
    json_kv_int(js, "access_group", (long)z->user1.Access);

    if (z->user1.Access >= 0 && z->user1.Access < 32) {
        json_kv_str(js, "group_name",
            strip_mci(buf, sizeof(buf),
                myp->AGC[(int)z->user1.Access].Name));
    } else {
        json_kv_null(js, "group_name");
    }

    /* Location (same logic as cmd_who) */
    if (z->MyDoing && z->MyDoing[0]) {
        json_kv_str(js, "location",
            strip_mci(buf, sizeof(buf), z->MyDoing));
    } else if (z->Doing[0]) {
        json_kv_str(js, "location",
            strip_mci(buf, sizeof(buf), z->Doing));
    } else {
        json_kv_null(js, "location");
    }

    /* Time fields (raw tenths for precision) */
    json_kv_int(js, "time_online_tenths", (long)z->TimeOnLine);
    json_kv_int(js, "time_left_tenths", (long)z->TimeLeft);
    json_kv_uint(js, "idle_tenths", (unsigned long)z->TimeIdle);

    /* Additional detail */
    json_kv_int(js, "current_sub", (long)z->bn);
    json_kv_int(js, "carrier", (long)z->Carrier);
    json_kv_uint(js, "baud", (unsigned long)z->user1.BaudRate);
    json_kv_int(js, "caller_number", z->Caller);

    json_obj_close(js);
}

int cmd_who_detail(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int i;

    /*
     * argv[0] = "who", argv[1] = "--detail" or numeric port
     *
     * Caller ensures argc >= 2.
     */
    if (argc < 2) {
        json_error("Usage: cnet-cli who [--detail | <port>]");
        return 1;
    }

    if (strcmp(argv[1], "--detail") == 0) {
        /* All online ports, extended info */
        json_init(&js, stdout);
        json_obj_open(&js);

        json_key(&js, "users");
        json_arr_open(&js);

        for (i = 0; i <= (int)myp->HiPort && i < 100; i++) {
            struct PortData *z = myp->PortZ[i];

            if (!z || z == myp->z0) continue;
            if (!z->OnLine) continue;

            emit_port_detail(&js, z, myp);
        }

        json_arr_close(&js);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    if (all_digits(argv[1])) {
        /* Single port detail */
        int port = atoi(argv[1]);
        struct PortData *z;

        if (port < 0 || port > (int)myp->HiPort || port >= 100) {
            json_error("Port number out of range");
            return 1;
        }

        z = myp->PortZ[port];
        if (!z || z == myp->z0) {
            json_error("Port not loaded");
            return 1;
        }

        if (!z->OnLine) {
            json_error("Port is not online");
            return 1;
        }

        json_init(&js, stdout);
        emit_port_detail(&js, z, myp);
        json_finish(&js);
        return 0;
    }

    json_error("Usage: cnet-cli who [--detail | <port>]");
    return 1;
}

/* ---- olm send ---- */

/*
 * Send an OLM (On-Line Message) to a user on a specific port.
 *
 * Usage: cnet-cli olm <port> --from <account> --text "message"
 *                              [--broadcast]
 *
 * <port>        Target port number (where recipient is logged in)
 * --from        Sender account number or handle
 * --text        Message text (max 380 chars)
 * --broadcast   Set OLM_BROADCAST flag (0x00000001)
 *
 * Calls FileOLM(z, text, sender_account, flags) from cnet.library.
 */
int cmd_olm_send(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct PortData *z;
    const char *from_arg = NULL;
    const char *text_arg = NULL;
    int broadcast = 0;
    int port;
    short sender_acct;
    long flags;
    UBYTE result;
    ULONG sender_id;
    char handle_buf[128];
    char sender_buf[128];
    int i;

    /*
     * argv[0] is the first argument after "olm" (the port number).
     * Minimum: <port> --from <acct> --text "msg" = 5 args.
     */
    if (argc < 5) {
        json_error("Usage: cnet-cli olm <port>"
            " --from <account> --text \"message\""
            " [--broadcast]");
        return 1;
    }

    /* Parse port number (argv[0]) */
    if (!all_digits(argv[0])) {
        json_error("Port must be a number");
        return 1;
    }
    port = atoi(argv[0]);

    /* Parse named arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--from") == 0) {
            if (i + 1 >= argc) {
                json_error("--from requires an argument");
                return 1;
            }
            from_arg = argv[++i];
        } else if (strcmp(argv[i], "--text") == 0) {
            if (i + 1 >= argc) {
                json_error("--text requires an argument");
                return 1;
            }
            text_arg = argv[++i];
        } else if (strcmp(argv[i], "--broadcast") == 0) {
            broadcast = 1;
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Unknown option: %s", argv[i]);
            json_error(buf);
            return 1;
        }
    }

    if (!from_arg) {
        json_error("--from is required");
        return 1;
    }
    if (!text_arg) {
        json_error("--text is required");
        return 1;
    }

    /* Validate text length (OLM max is 380 chars) */
    if (strlen(text_arg) > 380) {
        json_error("Message text exceeds 380 character limit");
        return 1;
    }

    /* Validate port range */
    if (port < 0 || port > (int)myp->HiPort || port >= 100) {
        json_error("Port number out of range");
        return 1;
    }

    /* Get PortData for target port */
    z = myp->PortZ[port];
    if (!z || z == myp->z0) {
        json_error("Port not loaded");
        return 1;
    }
    if (!z->OnLine) {
        json_error("No user online on that port");
        return 1;
    }

    /* Resolve sender account */
    sender_acct = resolve_user_full(myp, from_arg, NULL, 0);
    if (sender_acct < 1) {
        json_error("Sender account not found");
        return 1;
    }

    /* Build flags */
    flags = broadcast ? OLM_BROADCAST : 0;

    /* Try FileOLM from cnet.library first */
    result = FileOLM(z, (char *)text_arg, sender_acct, flags);

    /* Resolve sender handle and IDNumber under SEM[1] (needed by both paths) */
    sender_id = 0;
    {
        char sender_raw[128];
        sender_raw[0] = '\0';
        ObtainSemaphoreShared(&myp->SEM[1]);
        if (sender_acct >= 1
            && sender_acct <= myp->Nums[NUMS_CURRENT_ACCOUNTS]) {
            strncpy(sender_raw,
                myp->Key[sender_acct - 1].Handle,
                sizeof(sender_raw) - 1);
            sender_raw[sizeof(sender_raw) - 1] = '\0';
            sender_id = (ULONG)myp->Key[sender_acct - 1].IDNumber;
        }
        ReleaseSemaphore(&myp->SEM[1]);
        strip_mci(sender_buf, sizeof(sender_buf), sender_raw);
    }

    /*
     * If FileOLM failed (returns 0), fall back to direct file I/O.
     * FileOLM only works when the sender is account #1 (sysop) from
     * standalone CLI.  For other senders, write the OLM file directly.
     */
    if (!result) {
        unsigned char hdr[64];
        char path[256];
        BPTR fh;
        size_t text_len;

        /* Build 64-byte OLM header in raw buffer */
        memset(hdr, 0, sizeof(hdr));

        /* Offset 0: ByID (ULONG, 4 bytes, big-endian) */
        hdr[0] = (unsigned char)(sender_id >> 24);
        hdr[1] = (unsigned char)(sender_id >> 16);
        hdr[2] = (unsigned char)(sender_id >> 8);
        hdr[3] = (unsigned char)(sender_id);

        /* Offset 4: ByUser (char[26], null-terminated) */
        strncpy((char *)&hdr[4], sender_buf, 25);
        hdr[29] = '\0';

        /* Offset 30: ByAccount (short, 2 bytes, big-endian) */
        hdr[30] = (unsigned char)((unsigned short)sender_acct >> 8);
        hdr[31] = (unsigned char)((unsigned short)sender_acct);

        /* Offset 32: Port (short, 2 bytes, big-endian) */
        hdr[32] = (unsigned char)((unsigned short)port >> 8);
        hdr[33] = (unsigned char)((unsigned short)port);

        /* Offset 34: broadcast (UBYTE) */
        hdr[34] = broadcast ? 1 : 0;

        /* Offset 36: date (LONG, 4 bytes, big-endian) -- UTC */
        /* Offset 35 is padding between UBYTE broadcast and LONG date */
        {
            unsigned long now;

            if (CNet4Base) {
                /* CNetTime() returns the BBS-native timestamp matching
                 * the format FileOLM writes to the OLM date field.
                 * Empirically this is Unix epoch (seconds since
                 * 1970-01-01), despite the SDK documenting Amiga epoch. */
                now = (unsigned long)CNetTime();
            } else {
                /* Fallback: DateStamp() + epoch math. This has a
                 * timezone offset bug (DateStamp returns local time,
                 * not UTC), but is better than no timestamp at all. */
                struct DateStamp ds;
                DateStamp(&ds);
                now = 252460800UL
                    + (unsigned long)ds.ds_Days * 86400UL
                    + (unsigned long)ds.ds_Minute * 60UL
                    + (unsigned long)ds.ds_Tick / 50UL;
            }
            hdr[36] = (unsigned char)(now >> 24);
            hdr[37] = (unsigned char)(now >> 16);
            hdr[38] = (unsigned char)(now >> 8);
            hdr[39] = (unsigned char)(now);
        }

        /* Offsets 40-63: other[23] + padding -- already zeroed */

        /* Build file path: {OLMpath}_aolm{port} */
        snprintf(path, sizeof(path), "%s_aolm%d",
            myp->gc.OLMpath, port);

        /* Write OLM file using AmigaOS file I/O */
        fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
        if (!fh) {
            /* If the file exists but we can't open it, CNet is
             * holding a lock while displaying a pending OLM. */
            BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
            if (lock) {
                UnLock(lock);
                json_error("Target has pending OLM");
            } else {
                json_error("Failed to create OLM file");
            }
            return 1;
        }

        text_len = strlen(text_arg);

        if (Write(fh, hdr, 64) != 64
            || (text_len > 0
                && Write(fh, (APTR)text_arg, text_len) != (LONG)text_len)) {
            Close(fh);
            json_error("Failed to write OLM file");
            return 1;
        }

        /* Write OLM text terminator: LF + Ctrl-Z EOF + LF */
        {
            static const char olm_term[] = "\n\x1a\n";
            Write(fh, (APTR)olm_term, 3);
        }

        Close(fh);

        /* Increment OLMWaiting counter on target port */
        z->OLMWaiting++;
    }

    /* Success output */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "sent");
    json_kv_str(&js, "method", result ? "fileolm" : "direct_io");
    json_kv_int(&js, "port", (long)port);
    json_kv_str(&js, "to_handle",
        strip_mci(handle_buf, sizeof(handle_buf), z->user1.Handle));
    json_kv_int(&js, "to_account", (long)z->id);
    json_kv_str(&js, "from_handle", sender_buf);
    json_kv_int(&js, "from_account", (long)sender_acct);
    if (broadcast)
        json_kv_bool(&js, "broadcast", 1);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- user delete ---- */

/*
 * File-scope pointer to Key[] array, used by qsort comparators.
 * Set before each qsort call, used only within the sort callback.
 * Standard C pattern for passing context to qsort comparators.
 */
static struct KeyElement4 *sort_key_ptr;

/*
 * Case-insensitive string comparison for IName[] sort.
 * Each entry is a short:
 *   positive = handle reference (account# = value)
 *   negative = realname reference (account# = -value)
 */
static int iname_compare(const void *a, const void *b)
{
    short va = *(const short *)a;
    short vb = *(const short *)b;
    const char *sa;
    const char *sb;

    if (va > 0)
        sa = sort_key_ptr[va - 1].Handle;
    else
        sa = sort_key_ptr[(-va) - 1].RealName;

    if (vb > 0)
        sb = sort_key_ptr[vb - 1].Handle;
    else
        sb = sort_key_ptr[(-vb) - 1].RealName;

    return strcasecmp(sa, sb);
}

/*
 * Phone number comparison for IPhone[] sort.
 * Sort by phone1 ascending, then phone2 ascending.
 */
static int iphone_compare(const void *a, const void *b)
{
    short va = *(const short *)a;
    short vb = *(const short *)b;
    long p1a = sort_key_ptr[va - 1].phone1;
    long p1b = sort_key_ptr[vb - 1].phone1;
    long p2a = sort_key_ptr[va - 1].phone2;
    long p2b = sort_key_ptr[vb - 1].phone2;

    if (p1a < p1b) return -1;
    if (p1a > p1b) return 1;
    if (p2a < p2b) return -1;
    if (p2a > p2b) return 1;
    return 0;
}

/*
 * Rebuild IName[] index from Key[] array.
 * Returns the number of entries written into myp->IName[],
 * or -1 on allocation failure.
 */
int rebuild_iname_index(struct MainPort *myp)
{
    long num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];
    int max_entries = (int)num_accounts * 2;
    short *temp;
    int n = 0;
    int i;

    temp = (short *)malloc((size_t)max_entries * sizeof(short));
    if (!temp)
        return -1;

    for (i = 0; i < (int)num_accounts; i++) {
        if (myp->Key[i].Handle[0] == '\0')
            continue;
        temp[n++] = (short)(i + 1);
        if (myp->Key[i].RealName[0] && !myp->Key[i].PName)
            temp[n++] = (short)(-(i + 1));
    }

    sort_key_ptr = myp->Key;
    qsort(temp, (size_t)n, sizeof(short), iname_compare);

    for (i = 0; i < n; i++)
        myp->IName[i] = temp[i];

    free(temp);
    return n;
}

/*
 * Rebuild IPhone[] index from Key[] array.
 * Returns the number of entries written into myp->IPhone[],
 * or -1 on allocation failure.
 */
int rebuild_iphone_index(struct MainPort *myp)
{
    long num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];
    short *temp;
    int n = 0;
    int i;

    temp = (short *)malloc((size_t)num_accounts * sizeof(short));
    if (!temp)
        return -1;

    for (i = 0; i < (int)num_accounts; i++) {
        if (myp->Key[i].Handle[0] == '\0')
            continue;
        temp[n++] = (short)(i + 1);
    }

    sort_key_ptr = myp->Key;
    qsort(temp, (size_t)n, sizeof(short), iphone_compare);

    for (i = 0; i < n; i++)
        myp->IPhone[i] = temp[i];

    free(temp);
    return n;
}

/*
 * Write user index files to disk.
 *
 * Writes bbs.ukeys4, bbs.uind1, bbs.uind2, bbs.sdata.
 * Must be called while holding SEM[1].
 *
 * Returns a bitmask of successfully written files:
 *   bit 0 = bbs.ukeys4
 *   bit 1 = bbs.uind1
 *   bit 2 = bbs.uind2
 *   bit 3 = bbs.sdata
 */
int write_user_index_files(struct MainPort *myp,
    int iname_count, int iphone_count)
{
    long num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];
    int written_mask = 0;
    BPTR fh;
    long nbytes;
    long result;

    /* bbs.ukeys4 */
    nbytes = num_accounts * (long)sizeof(struct KeyElement4);
    fh = Open((CONST_STRPTR)"sysdata:bbs.ukeys4", MODE_NEWFILE);
    if (fh) {
        result = Write(fh, (APTR)myp->Key, nbytes);
        Close(fh);
        if (result == nbytes)
            written_mask |= 1;
    }

    /* bbs.uind1 */
    nbytes = (long)iname_count * (long)sizeof(short);
    fh = Open((CONST_STRPTR)"sysdata:bbs.uind1", MODE_NEWFILE);
    if (fh) {
        result = Write(fh, (APTR)myp->IName, nbytes);
        Close(fh);
        if (result == nbytes)
            written_mask |= 2;
    }

    /* bbs.uind2 */
    nbytes = (long)iphone_count * (long)sizeof(short);
    fh = Open((CONST_STRPTR)"sysdata:bbs.uind2", MODE_NEWFILE);
    if (fh) {
        result = Write(fh, (APTR)myp->IPhone, nbytes);
        Close(fh);
        if (result == nbytes)
            written_mask |= 4;
    }

    /* bbs.sdata -- Nums[0..4], 5 longs = 20 bytes */
    fh = Open((CONST_STRPTR)"sysdata:bbs.sdata", MODE_NEWFILE);
    if (fh) {
        result = Write(fh, (APTR)myp->Nums, 5L * (long)sizeof(long));
        Close(fh);
        if (result == 5L * (long)sizeof(long))
            written_mask |= 8;
    }

    return written_mask;
}

int cmd_user_delete(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    struct UserData *user;
    struct PortData *online_z;
    int force = 0;
    int i;
    int iname_count;
    int iphone_count;
    int written_mask;
    char saved_handle[22];
    char saved_realname[27];
    char saved_uucp[12];
    int saved_account;
    char buf[128];

    /* Step 1: Parse args, require --force */
    if (argc < 2) {
        json_error("Usage: cnet-cli user delete"
            " <account|handle> --force");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0)
            force = 1;
    }

    if (!force) {
        json_error("--force flag required for destructive"
            " delete operation");
        return 1;
    }

    /* Step 2: Resolve user */
    account = resolve_user(myp, argv[1]);
    if (account < 0) {
        json_error("User not found");
        return 1;
    }

    /* Step 3: Sysop check */
    if (account == 1) {
        json_error("Cannot delete account #1 (sysop)");
        return 1;
    }

    /* Step 4: Online check */
    online_z = IsNowOnLine(myp, account);
    if (online_z) {
        snprintf(buf, sizeof(buf),
            "Cannot delete: user is online on port %d",
            (int)online_z->InPort);
        json_error(buf);
        return 1;
    }

    /* Step 5: LockAccount */
    user = LockAccount(account);
    if (!user) {
        json_error("Cannot lock account (invalid or in use)");
        return 1;
    }

    /* Step 6: Empty slot check */
    if (user->Handle[0] == '\0') {
        UnLockAccount(account, 0);
        json_error("Account slot is already empty");
        return 1;
    }

    /* Step 7: Save identity fields for JSON output */
    strip_mci(saved_handle, sizeof(saved_handle), user->Handle);
    strip_mci(saved_realname, sizeof(saved_realname),
        user->RealName);
    safe_strcpy(saved_uucp, user->UUCP, (int)sizeof(saved_uucp));
    saved_account = (int)account;

    /* Step 8: Zero UserData fields */
    user->Handle[0] = '\0';
    user->RealName[0] = '\0';
    user->UUCP[0] = '\0';
    user->PhoneNo[0] = '\0';
    user->VoiceNo[0] = '\0';
    user->IDNumber = 0;
    user->Banner[0] = '\0';
    user->Address[0] = '\0';
    user->CityState[0] = '\0';
    user->PassWord[0] = '\0';

    /* Step 9: UnLockAccount with save=1 */
    UnLockAccount(account, 1);

    /* Step 10: Exclusive lock on Key[]/IName[]/IPhone[] */
    ObtainSemaphore(&myp->SEM[1]);

    /* Step 11: Clear Key[account-1] */
    myp->Key[account - 1].Handle[0] = '\0';
    myp->Key[account - 1].RealName[0] = '\0';
    myp->Key[account - 1].UUCP[0] = '\0';
    myp->Key[account - 1].IDNumber = 0;
    myp->Key[account - 1].phone1 = 0;
    myp->Key[account - 1].phone2 = 0;
    myp->Key[account - 1].Access = 0;
    myp->Key[account - 1].PName = 0;

    /* Step 12: Rebuild IName[] */
    iname_count = rebuild_iname_index(myp);

    /* Step 13: Rebuild IPhone[] */
    iphone_count = rebuild_iphone_index(myp);

    /* Step 14: Lock Nums */
    ObtainSemaphore(&myp->SEM[4]);

    /* Step 15: Decrement in-use accounts */
    myp->Nums[NUMS_INUSE_ACCOUNTS]--;

    /* Step 16: Release Nums lock */
    ReleaseSemaphore(&myp->SEM[4]);

    /* Step 17: Write index files (inside SEM[1]) */
    if (iname_count < 0 || iphone_count < 0)
        written_mask = 0;
    else
        written_mask = write_user_index_files(myp,
            iname_count, iphone_count);

    /* Step 18: Release SEM[1] */
    ReleaseSemaphore(&myp->SEM[1]);

    /* Step 19: Output success JSON */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "deleted");
    json_kv_int(&js, "account", (long)saved_account);
    json_kv_str(&js, "handle", saved_handle);
    json_kv_str(&js, "real_name", saved_realname);
    json_kv_str(&js, "uucp", saved_uucp);

    json_key(&js, "files_written");
    json_arr_open(&js);
    if (written_mask & 1)
        json_str(&js, "bbs.ukeys4");
    if (written_mask & 2)
        json_str(&js, "bbs.uind1");
    if (written_mask & 4)
        json_str(&js, "bbs.uind2");
    if (written_mask & 8)
        json_str(&js, "bbs.sdata");
    json_arr_close(&js);

    json_key(&js, "warnings");
    json_arr_open(&js);
    if (iname_count < 0)
        json_str(&js, "IName rebuild failed (out of memory)");
    if (iphone_count < 0)
        json_str(&js, "IPhone rebuild failed (out of memory)");
    if (iname_count >= 0 && iphone_count >= 0) {
        if (!(written_mask & 1))
            json_str(&js, "Failed to write bbs.ukeys4");
        if (!(written_mask & 2))
            json_str(&js, "Failed to write bbs.uind1");
        if (!(written_mask & 4))
            json_str(&js, "Failed to write bbs.uind2");
        if (!(written_mask & 8))
            json_str(&js, "Failed to write bbs.sdata");
    }
    json_arr_close(&js);

    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
