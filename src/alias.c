/*
 * alias.c -- Mail alias commands for cnet-cli
 *
 * Mail alias management: list, add, remove.
 *
 * Aliases are stored in mail:users/{uucp}/aliases as a flat array
 * of 134-byte MailAlias records. No file header. The struct layout
 * was reconstructed from binary analysis of live CNet 5.36c data
 * (see research/mail_alias_format.md).
 *
 * Mutation commands (add/remove) acquire the per-account mail
 * semaphore via GetMailSems() from cnetmail.library. List uses
 * a shared semaphore lock.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "alias.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;
extern struct Library *CNetMailBase;

/* ---- On-disk mail alias layout (134 bytes) ---- */

/*
 * Reconstructed from binary analysis. NOT in the SDK -- mail.h
 * only forward-declares struct MailAlias. See research/mail_alias_format.md.
 */
struct MailAlias {
    char   Alias[24];          /*   0: shortcut name (max 20 usable chars) */
    char   Name[24];           /*  24: recipient handle or name (max 23 chars) */
    char   Address[80];        /*  48: network address or blank for local */
    UBYTE  expansion[2];       /* 128: reserved, zero-fill */
    UBYTE  next_ptr[4];        /* 130: stale runtime list pointer, zero-fill */
};

_Static_assert(sizeof(struct MailAlias) == 134,
    "MailAlias must be 134 bytes (on-disk format)");

#define ALIAS_RECORD_SIZE 134

/* ---- Alias type classification ---- */

static const char *alias_type(const struct MailAlias *a)
{
    if (a->Alias[0] == '+' && a->Alias[1] == '\0')
        return "forward";
    if (a->Address[0] != '\0')
        return "network";
    return "local";
}

/* ---- Path construction ---- */

static void build_alias_path(char *buf, int bufsz, const char *uucp)
{
    snprintf(buf, bufsz, "mail:users/%s/aliases", uucp);
}

/* ---- User resolution (same pattern as mail.c resolve_user_mail) ---- */

static int resolve_alias_user(struct MainPort *myp,
    const char *id_or_handle, short *out_account,
    char *out_uucp, int uucp_bufsz)
{
    short account;

    account = resolve_user_full(myp, id_or_handle,
        out_uucp, uucp_bufsz);
    if (account < 1) {
        char buf[256];
        snprintf(buf, sizeof(buf), "User not found: %s", id_or_handle);
        json_error(buf);
        return -1;
    }
    if (out_uucp[0] == '\0') {
        char buf[256];
        snprintf(buf, sizeof(buf), "No UUCP name for account %d",
            (int)account);
        json_error(buf);
        return -1;
    }

    *out_account = account;
    return 0;
}

/* ---- mail alias list ---- */

int cmd_mail_alias_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct MailAlias rec;
    short account;
    char uucp[12];
    char path[256];
    BPTR fh;
    long fsize;
    long total;
    int index = 0;
    struct SignalSemaphore *sems = NULL;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail alias list <account|handle>");
        return 1;
    }

    if (resolve_alias_user(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0)
        return 1;

    build_alias_path(path, (int)sizeof(path), uucp);

    /* Shared lock for read */
    if (CNetMailBase) {
        sems = GetMailSems();
        if (sems)
            ObtainSemaphoreShared(&sems[account - 1]);
    }

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        /* No alias file -- emit empty result */
        if (sems)
            ReleaseSemaphore(&sems[account - 1]);

        json_init(&js, stdout);
        json_obj_open(&js);
        json_key(&js, "aliases");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_kv_int(&js, "account", (long)account);
        json_kv_str(&js, "uucp_name", uucp);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    /* Determine file size and record count */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    total = fsize / (long)ALIAS_RECORD_SIZE;

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "aliases");
    json_arr_open(&js);

    while (Read(fh, &rec, (long)ALIAS_RECORD_SIZE)
            == (long)ALIAS_RECORD_SIZE) {
        /* Ensure null termination for safety */
        rec.Alias[23] = '\0';
        rec.Name[23] = '\0';
        rec.Address[79] = '\0';

        json_obj_open(&js);
        json_kv_int(&js, "index", (long)index);
        json_kv_str(&js, "alias", rec.Alias);
        json_kv_str(&js, "name", rec.Name);
        json_kv_str(&js, "address", rec.Address);
        json_kv_str(&js, "type", alias_type(&rec));
        json_obj_close(&js);

        index++;
    }

    Close(fh);

    if (sems)
        ReleaseSemaphore(&sems[account - 1]);

    json_arr_close(&js);
    json_kv_int(&js, "count", (long)total);
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "uucp_name", uucp);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail alias add ---- */

int cmd_mail_alias_add(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct MailAlias rec;
    short account;
    char uucp[12];
    char path[256];
    const char *alias_name = NULL;
    const char *name = NULL;
    const char *address = "";
    BPTR fh;
    long fsize;
    long total;
    struct SignalSemaphore *sems;
    int i;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail alias add <account|handle>"
            " --alias <name> --name <recipient> [--address <addr>]");
        return 1;
    }

    /* Parse flags */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--alias") == 0 && i + 1 < argc) {
            i++;
            alias_name = argv[i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            i++;
            name = argv[i];
        } else if (strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
            i++;
            address = argv[i];
        }
    }

    if (!alias_name) {
        json_error("Missing required --alias flag");
        return 1;
    }
    if (!name) {
        json_error("Missing required --name flag");
        return 1;
    }

    /* Validate field lengths */
    if (strlen(alias_name) > 20) {
        json_error("Alias name exceeds 20 characters");
        return 1;
    }
    if (strlen(name) > 23) {
        json_error("Name exceeds 23 characters");
        return 1;
    }
    if (strlen(address) > 79) {
        json_error("Address exceeds 79 characters");
        return 1;
    }

    if (resolve_alias_user(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0)
        return 1;

    /* Require cnetmail.library for mutation locking */
    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    sems = GetMailSems();
    if (!sems) {
        json_error("mail semaphores not available");
        return 1;
    }

    build_alias_path(path, (int)sizeof(path), uucp);

    /* Build the record -- zero-fill first */
    memset(&rec, 0, sizeof(rec));
    safe_strcpy(rec.Alias, alias_name, (int)sizeof(rec.Alias));
    safe_strcpy(rec.Name, name, (int)sizeof(rec.Name));
    safe_strcpy(rec.Address, address, (int)sizeof(rec.Address));
    /* expansion and next_ptr already zeroed by memset */

    /* Acquire exclusive lock */
    ObtainSemaphore(&sems[account - 1]);

    /* Open for append (MODE_READWRITE creates if needed) */
    fh = Open((CONST_STRPTR)path, MODE_READWRITE);
    if (!fh) {
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Failed to open alias file for writing");
        return 1;
    }

    /* Seek to end to append */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_CURRENT);

    if (Write(fh, &rec, (long)ALIAS_RECORD_SIZE)
            != (long)ALIAS_RECORD_SIZE) {
        Close(fh);
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Failed to write alias record");
        return 1;
    }

    Close(fh);

    total = (fsize / (long)ALIAS_RECORD_SIZE) + 1;

    ReleaseSemaphore(&sems[account - 1]);

    /* Emit success response */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "added");
    json_kv_str(&js, "alias", alias_name);
    json_kv_str(&js, "name", name);
    json_kv_str(&js, "address", address);
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "uucp_name", uucp);
    json_kv_int(&js, "total_aliases", total);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail alias remove ---- */

int cmd_mail_alias_remove(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char path[256];
    const char *alias_name = NULL;
    const char *filter_name = NULL;
    BPTR fh;
    long fsize;
    long total;
    long kept;
    long removed;
    struct SignalSemaphore *sems;
    struct MailAlias *records = NULL;
    long nrecs;
    long ri;
    int i;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail alias remove <account|handle>"
            " --alias <name> [--name <recipient>]");
        return 1;
    }

    /* Parse flags */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--alias") == 0 && i + 1 < argc) {
            i++;
            alias_name = argv[i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            i++;
            filter_name = argv[i];
        }
    }

    if (!alias_name) {
        json_error("Missing required --alias flag");
        return 1;
    }

    if (resolve_alias_user(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0)
        return 1;

    /* Require cnetmail.library for mutation locking */
    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    sems = GetMailSems();
    if (!sems) {
        json_error("mail semaphores not available");
        return 1;
    }

    build_alias_path(path, (int)sizeof(path), uucp);

    /* Acquire exclusive lock */
    ObtainSemaphore(&sems[account - 1]);

    /* Read all records */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&sems[account - 1]);
        json_error("No aliases found for user");
        return 1;
    }

    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    nrecs = fsize / (long)ALIAS_RECORD_SIZE;

    if (nrecs <= 0) {
        Close(fh);
        ReleaseSemaphore(&sems[account - 1]);
        json_error("No aliases found for user");
        return 1;
    }

    records = (struct MailAlias *)AllocMem(
        (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE), 0);
    if (!records) {
        Close(fh);
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Out of memory");
        return 1;
    }

    if (Read(fh, records, nrecs * (long)ALIAS_RECORD_SIZE)
            != nrecs * (long)ALIAS_RECORD_SIZE) {
        Close(fh);
        FreeMem(records, (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE));
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Failed to read alias file");
        return 1;
    }

    Close(fh);

    /* Ensure null termination on all records */
    for (ri = 0; ri < nrecs; ri++) {
        records[ri].Alias[23] = '\0';
        records[ri].Name[23] = '\0';
        records[ri].Address[79] = '\0';
    }

    /* Count matching records */
    removed = 0;
    for (ri = 0; ri < nrecs; ri++) {
        if (strcasecmp(records[ri].Alias, alias_name) == 0) {
            if (filter_name &&
                    strcasecmp(records[ri].Name, filter_name) != 0)
                continue;
            removed++;
        }
    }

    if (removed == 0) {
        FreeMem(records, (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE));
        ReleaseSemaphore(&sems[account - 1]);
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Alias not found: %s",
                alias_name);
            json_error(buf);
        }
        return 1;
    }

    /* Rewrite file without matching records */
    kept = nrecs - removed;

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        FreeMem(records, (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE));
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Failed to write alias file");
        return 1;
    }

    for (ri = 0; ri < nrecs; ri++) {
        int match;

        match = (strcasecmp(records[ri].Alias, alias_name) == 0);
        if (match && filter_name)
            match = (strcasecmp(records[ri].Name, filter_name) == 0);

        if (match)
            continue;  /* skip this record */

        if (Write(fh, &records[ri], (long)ALIAS_RECORD_SIZE)
                != (long)ALIAS_RECORD_SIZE) {
            Close(fh);
            FreeMem(records,
                (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE));
            ReleaseSemaphore(&sems[account - 1]);
            json_error("Failed to write alias file");
            return 1;
        }
    }

    Close(fh);
    FreeMem(records, (ULONG)(nrecs * (long)ALIAS_RECORD_SIZE));

    total = kept;

    ReleaseSemaphore(&sems[account - 1]);

    /* Emit success response */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "removed");
    json_kv_str(&js, "alias", alias_name);
    json_kv_int(&js, "removed_count", removed);
    json_kv_int(&js, "account", (long)account);
    json_kv_str(&js, "uucp_name", uucp);
    json_kv_int(&js, "total_aliases", total);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
