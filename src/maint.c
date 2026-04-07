/*
 * maint.c -- Maintenance commands for cnet-cli
 *
 * maint pointers    -- Rebuild user index files (IName, IPhone, ukeys4)
 * maint count       -- Recount subboard/mail counts
 * maint repair-mail -- Mail file compaction and header fixup
 * maint repair-sub  -- Subboard text pool compaction (not yet implemented)
 *
 * Mail header fixup: unknown_0 (offset +0) in MailHeaderDisk should always
 * be zero. write_mail_direct() and cmd_mail_delete() were erroneously
 * writing non-zero values. Fixed in mail.c; repair-mail also zeroes
 * existing non-zero values during compaction.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>

#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "maint.h"
#include "user.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;
extern struct Library *CNetMailBase;

/*
 * cmd_maint_pointers -- Rebuild user index files.
 *
 * Rebuilds IName[] and IPhone[] sorted index arrays from the in-memory
 * Key[] array, then writes all index files to disk:
 *   sysdata:bbs.ukeys4  -- raw Key[] dump
 *   sysdata:bbs.uind1   -- IName[] sorted by handle/realname
 *   sysdata:bbs.uind2   -- IPhone[] sorted by phone number
 *   sysdata:bbs.sdata   -- Nums[] array (5 longs)
 *
 * The operation is idempotent and non-destructive to primary data.
 * Requires SEM[1] exclusive for the duration.
 */
int cmd_maint_pointers(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int iname_count;
    int iphone_count;
    int written_mask;

    (void)argc;
    (void)argv;

    /* Acquire exclusive lock on Key[]/IName[]/IPhone[] */
    ObtainSemaphore(&myp->SEM[1]);

    /* Rebuild sorted index arrays from Key[] */
    iname_count = rebuild_iname_index(myp);
    iphone_count = rebuild_iphone_index(myp);

    /* Write index files to disk */
    if (iname_count < 0 || iphone_count < 0)
        written_mask = 0;
    else
        written_mask = write_user_index_files(myp,
            iname_count, iphone_count);

    ReleaseSemaphore(&myp->SEM[1]);

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_str(&js, "command", "maint_pointers");
    json_kv_int(&js, "accounts", myp->Nums[NUMS_CURRENT_ACCOUNTS]);
    if (iname_count >= 0)
        json_kv_int(&js, "iname_entries", (long)iname_count);
    else
        json_kv_null(&js, "iname_entries");
    if (iphone_count >= 0)
        json_kv_int(&js, "iphone_entries", (long)iphone_count);
    else
        json_kv_null(&js, "iphone_entries");

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

/*
 * Stat a file and return its size in bytes.
 * Returns file size >= 0 on success, -1 on failure.
 */
static long stat_file_size(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    long size;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock)
        return -1;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return -1;
    }

    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return -1;
    }

    size = fib->fib_Size;
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return size;
}

/*
 * Recount a single subboard's item/response counts from its data files.
 *
 * Reads _Items3, _Headers3, and _Message3 directly from disk (does NOT
 * use OneMoreUser/OneLessUser) to compute ground-truth values for:
 *   sub->rn      -- item record count from _Items3 file size
 *   sub->nm      -- message record count from _Message3 file size
 *   sub->count   -- next unique ID from max(ItemHeader.Number, Message.Number)+1
 *   sub->nNewMess -- runtime buffer, unconditionally cleared to 0
 *   ihead[i].Responses -- per-item response count from _Message3 scan
 *
 * If apply is true, writes changes to disk:
 *   - sub->rn/nm/count/nNewMess via write_subboard_disk under SEM[5] exclusive
 *   - _Headers3 via direct file write under sub->sem exclusive (only if
 *     at least one Responses value changed)
 *
 * Emits per-subboard change details into the JSON changes array.
 *
 * Returns 1 if any values changed, 0 if no changes, -1 on error.
 */
static int recount_subboard(struct MainPort *myp, int physnum,
    int apply, struct json_state *js)
{
    struct SubboardType4 *sub;
    char data_path[96];
    char items_path[256];
    char headers_path[256];
    char msgs_path[256];
    char go_key[76];
    char title[61];
    char wbuf[128];
    long items_size, headers_size, msgs_size;
    USHORT new_rn;
    long new_nm;
    ULONG new_count;
    ULONG max_number;
    struct ItemHeader *iheads = NULL;
    struct MessageType3 *msgs = NULL;
    long n_headers, n_msgs;
    BPTR fh;
    long nread;
    int i;
    int any_changed = 0;
    int responses_fixed = 0;
    int headers_dirty = 0;
    USHORT old_rn;
    long old_nm;
    ULONG old_count;
    USHORT old_nNewMess;

    /* Read subboard metadata under SEM[5] shared */
    ObtainSemaphoreShared(&myp->SEM[5]);
    sub = &myp->Subboard[physnum];
    strncpy(data_path, sub->DataPath, sizeof(data_path) - 1);
    data_path[sizeof(data_path) - 1] = '\0';
    strncpy(go_key, sub->SubDirName, sizeof(go_key) - 1);
    go_key[sizeof(go_key) - 1] = '\0';
    strncpy(title, sub->Title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    old_rn = sub->rn;
    old_nm = sub->nm;
    old_count = sub->count;
    old_nNewMess = sub->nNewMess;
    ReleaseSemaphore(&myp->SEM[5]);

    /* Build paths to data files */
    build_data_file_path(items_path, sizeof(items_path),
        data_path, "_Items3");
    build_data_file_path(headers_path, sizeof(headers_path),
        data_path, "_Headers3");
    build_data_file_path(msgs_path, sizeof(msgs_path),
        data_path, "_Message3");

    /* Stat _Items3 */
    items_size = stat_file_size(items_path);
    if (items_size < 0) {
        /* File doesn't exist -- empty subboard */
        new_rn = 0;
    } else if (items_size % (long)sizeof(struct ItemType3) != 0) {
        snprintf(wbuf, sizeof(wbuf),
            "Sub %d: _Items3 size %ld not multiple of %d",
            physnum, items_size, (int)sizeof(struct ItemType3));
        warn_add(wbuf);
        return -1;
    } else {
        new_rn = (USHORT)(items_size / (long)sizeof(struct ItemType3));
    }

    /* Stat _Headers3 and verify consistency */
    headers_size = stat_file_size(headers_path);
    if (headers_size < 0) {
        n_headers = 0;
    } else {
        n_headers = headers_size / (long)sizeof(struct ItemHeader);
        if (headers_size % (long)sizeof(struct ItemHeader) != 0) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: _Headers3 size %ld not multiple of %d",
                physnum, headers_size, (int)sizeof(struct ItemHeader));
            warn_add(wbuf);
        }
    }
    if (n_headers != (long)new_rn && items_size >= 0 && headers_size >= 0) {
        snprintf(wbuf, sizeof(wbuf),
            "Sub %d: _Items3 records (%u) != _Headers3 records (%ld)",
            physnum, (unsigned)new_rn, n_headers);
        warn_add(wbuf);
    }

    /* Stat _Message3 */
    msgs_size = stat_file_size(msgs_path);
    if (msgs_size < 0) {
        new_nm = 0;
    } else if (msgs_size % (long)sizeof(struct MessageType3) != 0) {
        snprintf(wbuf, sizeof(wbuf),
            "Sub %d: _Message3 size %ld not multiple of %d",
            physnum, msgs_size, (int)sizeof(struct MessageType3));
        warn_add(wbuf);
        new_nm = msgs_size / (long)sizeof(struct MessageType3);
    } else {
        new_nm = msgs_size / (long)sizeof(struct MessageType3);
    }

    /* Compute new_count from max Number in headers and messages */
    max_number = 0;

    /* Read _Headers3 into memory */
    if (n_headers > 0) {
        iheads = (struct ItemHeader *)malloc(
            (unsigned long)n_headers * sizeof(struct ItemHeader));
        if (!iheads) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: malloc failed for %ld headers",
                physnum, n_headers);
            warn_add(wbuf);
            return -1;
        }
        fh = Open((CONST_STRPTR)headers_path, MODE_OLDFILE);
        if (!fh) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: cannot open _Headers3", physnum);
            warn_add(wbuf);
            free(iheads);
            return -1;
        }
        nread = Read(fh, (APTR)iheads,
            n_headers * (long)sizeof(struct ItemHeader));
        Close(fh);
        if (nread != n_headers * (long)sizeof(struct ItemHeader)) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: short read on _Headers3", physnum);
            warn_add(wbuf);
            free(iheads);
            return -1;
        }

        /* Scan for max Number */
        for (i = 0; i < (int)n_headers; i++) {
            if (iheads[i].Number > max_number)
                max_number = iheads[i].Number;
        }
    }

    /* Read _Message3 into memory */
    n_msgs = new_nm;
    if (n_msgs > 0) {
        msgs = (struct MessageType3 *)malloc(
            (unsigned long)n_msgs * sizeof(struct MessageType3));
        if (!msgs) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: malloc failed for %ld messages",
                physnum, n_msgs);
            warn_add(wbuf);
            if (iheads) free(iheads);
            return -1;
        }
        fh = Open((CONST_STRPTR)msgs_path, MODE_OLDFILE);
        if (!fh) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: cannot open _Message3", physnum);
            warn_add(wbuf);
            free(msgs);
            if (iheads) free(iheads);
            return -1;
        }
        nread = Read(fh, (APTR)msgs,
            n_msgs * (long)sizeof(struct MessageType3));
        Close(fh);
        if (nread != n_msgs * (long)sizeof(struct MessageType3)) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: short read on _Message3", physnum);
            warn_add(wbuf);
            free(msgs);
            if (iheads) free(iheads);
            return -1;
        }

        /* Scan for max Number in messages */
        for (i = 0; i < (int)n_msgs; i++) {
            if (msgs[i].Number > max_number)
                max_number = msgs[i].Number;
        }
    }

    /* new_count = max_number + 1, but never decrease from current */
    if (max_number > 0 || n_headers > 0 || n_msgs > 0)
        new_count = max_number + 1;
    else
        new_count = 0;

    /* SAFETY: count must NEVER decrease */
    if (new_count < old_count)
        new_count = old_count;

    /* Sanity check: warn if count jumps by more than 10x */
    if (old_count > 0 && new_count > 10 * old_count) {
        snprintf(wbuf, sizeof(wbuf),
            "Sub %d: count would jump from %lu to %lu (suspicious)",
            physnum, (unsigned long)old_count, (unsigned long)new_count);
        warn_add(wbuf);
    }

    /* Recount per-item responses from _Message3 */
    if (iheads && n_headers > 0) {
        for (i = 0; i < (int)n_headers; i++) {
            long resp_count = 0;
            int j;

            if (msgs && n_msgs > 0) {
                for (j = 0; j < (int)n_msgs; j++) {
                    if (msgs[j].ItemNumber == iheads[i].Number)
                        resp_count++;
                }
            }

            if (iheads[i].Responses != resp_count) {
                iheads[i].Responses = resp_count;
                headers_dirty = 1;
                responses_fixed++;
            }
        }
    }

    /* Determine if anything changed */
    if (new_rn != old_rn || new_nm != old_nm ||
        new_count != old_count || old_nNewMess != 0 ||
        headers_dirty)
        any_changed = 1;

    /* Emit JSON change entry */
    if (any_changed) {
        json_obj_open(js);
        json_kv_int(js, "physnum", (long)physnum);
        json_kv_str(js, "go_key", go_key);
        json_kv_str(js, "title", title);

        json_key(js, "rn");
        json_obj_open(js);
        json_kv_uint(js, "old", (unsigned long)old_rn);
        json_kv_uint(js, "new", (unsigned long)new_rn);
        json_obj_close(js);

        json_key(js, "nm");
        json_obj_open(js);
        json_kv_int(js, "old", old_nm);
        json_kv_int(js, "new", new_nm);
        json_obj_close(js);

        json_key(js, "count");
        json_obj_open(js);
        json_kv_uint(js, "old", (unsigned long)old_count);
        json_kv_uint(js, "new", (unsigned long)new_count);
        json_obj_close(js);

        json_key(js, "nNewMess");
        json_obj_open(js);
        json_kv_uint(js, "old", (unsigned long)old_nNewMess);
        json_kv_uint(js, "new", 0UL);
        json_obj_close(js);

        json_kv_int(js, "responses_fixed", (long)responses_fixed);
        json_obj_close(js);
    }

    /* Apply changes if requested */
    if (apply && any_changed) {
        /*
         * Write corrected _Headers3 under sub->sem exclusive.
         * Only if at least one Responses value was corrected.
         */
        if (headers_dirty && iheads && n_headers > 0) {
            struct SignalSemaphore *sem;

            ObtainSemaphoreShared(&myp->SEM[5]);
            sem = myp->Subboard[physnum].sem;
            ReleaseSemaphore(&myp->SEM[5]);

            if (sem) {
                ObtainSemaphore(sem);

                fh = Open((CONST_STRPTR)headers_path, MODE_NEWFILE);
                if (fh) {
                    nread = Write(fh, (APTR)iheads,
                        n_headers * (long)sizeof(struct ItemHeader));
                    Close(fh);
                    if (nread !=
                        n_headers * (long)sizeof(struct ItemHeader)) {
                        snprintf(wbuf, sizeof(wbuf),
                            "Sub %d: short write on _Headers3",
                            physnum);
                        warn_add(wbuf);
                    }
                } else {
                    snprintf(wbuf, sizeof(wbuf),
                        "Sub %d: cannot open _Headers3 for write",
                        physnum);
                    warn_add(wbuf);
                }

                /*
                 * Update in-memory ihead if subboard is loaded.
                 * sub->ihead is only valid when Users > 0 (loaded).
                 */
                {
                    struct SubboardType4 *s = &myp->Subboard[physnum];
                    if (s->ihead && s->Users > 0) {
                        int k;
                        long limit = (long)s->rn;
                        if (limit > n_headers)
                            limit = n_headers;
                        for (k = 0; k < (int)limit; k++)
                            s->ihead[k].Responses =
                                iheads[k].Responses;
                    }
                }

                ReleaseSemaphore(sem);
            }
        }

        /* Update subboard counters under SEM[5] exclusive */
        ObtainSemaphore(&myp->SEM[5]);
        sub = &myp->Subboard[physnum];
        sub->rn = new_rn;
        sub->nm = new_nm;
        sub->count = new_count;
        sub->nNewMess = 0;

        if (write_subboard_disk(physnum, sub) != 0) {
            snprintf(wbuf, sizeof(wbuf),
                "Sub %d: write_subboard_disk failed", physnum);
            warn_add(wbuf);
        }
        ReleaseSemaphore(&myp->SEM[5]);
    }

    if (msgs) free(msgs);
    if (iheads) free(iheads);

    return any_changed ? 1 : 0;
}

/*
 * cmd_maint_count -- Recount subboard and system counters.
 *
 * Two phases:
 *   Phase A: Per-subboard recount (rn, nm, count, nNewMess, Responses)
 *   Phase B: Nums[] recount (total accounts, in-use, high ID)
 *
 * Default mode is dry-run (report only). Use --apply to write changes.
 *
 * TOCTOU note: SEM[5] is not held during file I/O for individual
 * subboards. This is an accepted precedent consistent with message.c
 * (see message.c TOCTOU comment). Between the SEM[5]-shared read of
 * subboard metadata and the file stat/read, another process could
 * theoretically kill the subboard or modify files. The worst case is
 * reading stale paths, producing file-not-found warnings.
 */
int cmd_maint_count(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int apply = 0;
    int subs_only = 0;
    int nums_only = 0;
    short single_sub = -1;
    int i;
    int subs_scanned = 0;
    int subs_skipped = 0;
    int subs_changed = 0;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0) {
            apply = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            apply = 0;
        } else if (strcmp(argv[i], "--subs-only") == 0) {
            subs_only = 1;
        } else if (strcmp(argv[i], "--nums-only") == 0) {
            nums_only = 1;
        } else if (strcmp(argv[i], "--sub") == 0) {
            if (i + 1 >= argc) {
                json_error("--sub requires a subboard id or GO key");
                return 1;
            }
            i++;
            single_sub = resolve_subboard(myp, argv[i]);
            if (single_sub < 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "Subboard not found: %s", argv[i]);
                json_error(buf);
                return 1;
            }
            /* --sub implies --subs-only */
            subs_only = 1;
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Unknown option: %s", argv[i]);
            json_error(buf);
            return 1;
        }
    }

    if (subs_only && nums_only) {
        json_error("--subs-only and --nums-only are mutually exclusive");
        return 1;
    }

    warn_clear();

    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_str(&js, "command", "maint_count");
    json_kv_str(&js, "mode", apply ? "apply" : "dry-run");

    /* ---- Phase A: Subboard recount ---- */
    if (!nums_only) {
        int ns;
        int start, end;

        json_key(&js, "changes");
        json_arr_open(&js);

        ObtainSemaphoreShared(&myp->SEM[5]);
        ns = (int)myp->ns;
        ReleaseSemaphore(&myp->SEM[5]);

        if (single_sub >= 0) {
            start = (int)single_sub;
            end = (int)single_sub + 1;
        } else {
            start = 0;
            end = ns;
        }

        for (i = start; i < end; i++) {
            int marker;
            int subdirectory;
            int result;

            /* Check if subboard should be scanned */
            ObtainSemaphoreShared(&myp->SEM[5]);
            marker = (int)myp->Subboard[i].Marker;
            subdirectory = (int)myp->Subboard[i].Subdirectory;
            ReleaseSemaphore(&myp->SEM[5]);

            if (marker & MRK_SUBBOARD_KILLED) {
                subs_skipped++;
                continue;
            }
            if (subdirectory) {
                subs_skipped++;
                continue;
            }

            subs_scanned++;
            result = recount_subboard(myp, i, apply, &js);
            if (result > 0)
                subs_changed++;
            else if (result < 0)
                subs_skipped++;
        }

        json_arr_close(&js);

        json_kv_int(&js, "subboards_scanned", (long)subs_scanned);
        json_kv_int(&js, "subboards_skipped", (long)subs_skipped);
        json_kv_int(&js, "subboards_changed", (long)subs_changed);
    }

    /* ---- Phase B: Nums[] recount ---- */
    if (!subs_only) {
        long file_size;
        long new_total;
        long new_inuse;
        long new_high_id;
        long old_total;
        long old_inuse;
        long old_high_id;
        int nums_changed = 0;

        /* Stat bbs.udata4 for total account slots */
        file_size = stat_file_size("sysdata:bbs.udata4");
        if (file_size >= 0) {
            new_total = file_size / (long)sizeof(struct UserData);
        } else {
            new_total = -1;
        }

        /* Scan Key[] under SEM[1] shared */
        new_inuse = 0;
        new_high_id = 0;
        ObtainSemaphoreShared(&myp->SEM[1]);
        old_total = myp->Nums[NUMS_CURRENT_ACCOUNTS];
        old_inuse = myp->Nums[NUMS_INUSE_ACCOUNTS];
        old_high_id = myp->Nums[NUMS_HIGH_ID];
        {
            long accts = myp->Nums[NUMS_CURRENT_ACCOUNTS];
            int k;
            for (k = 0; k < (int)accts; k++) {
                if (myp->Key[k].Handle[0] != '\0') {
                    new_inuse++;
                    if (myp->Key[k].IDNumber > new_high_id)
                        new_high_id = myp->Key[k].IDNumber;
                }
            }
        }
        ReleaseSemaphore(&myp->SEM[1]);

        /* Nums[2] must NEVER decrease */
        if (new_high_id < old_high_id)
            new_high_id = old_high_id;

        /* If udata4 stat failed, keep the current value */
        if (new_total < 0)
            new_total = old_total;

        /* Emit Nums comparison */
        json_key(&js, "nums");
        json_obj_open(&js);

        json_key(&js, "current_accounts");
        json_obj_open(&js);
        json_kv_int(&js, "old", old_total);
        json_kv_int(&js, "new", new_total);
        json_kv_bool(&js, "changed", new_total != old_total);
        json_obj_close(&js);

        json_key(&js, "inuse_accounts");
        json_obj_open(&js);
        json_kv_int(&js, "old", old_inuse);
        json_kv_int(&js, "new", new_inuse);
        json_kv_bool(&js, "changed", new_inuse != old_inuse);
        json_obj_close(&js);

        json_key(&js, "high_id");
        json_obj_open(&js);
        json_kv_int(&js, "old", old_high_id);
        json_kv_int(&js, "new", new_high_id);
        json_kv_bool(&js, "changed", new_high_id != old_high_id);
        json_obj_close(&js);

        json_obj_close(&js);

        if (new_total != old_total || new_inuse != old_inuse ||
            new_high_id != old_high_id)
            nums_changed = 1;

        /* Apply Nums[] changes */
        if (apply && nums_changed) {
            BPTR fh;
            long result;

            ObtainSemaphore(&myp->SEM[1]);
            ObtainSemaphore(&myp->SEM[4]);

            myp->Nums[NUMS_CURRENT_ACCOUNTS] = new_total;
            myp->Nums[NUMS_INUSE_ACCOUNTS] = new_inuse;
            myp->Nums[NUMS_HIGH_ID] = new_high_id;

            fh = Open((CONST_STRPTR)"sysdata:bbs.sdata",
                MODE_NEWFILE);
            if (fh) {
                result = Write(fh, (APTR)myp->Nums,
                    5L * (long)sizeof(long));
                Close(fh);
                if (result != 5L * (long)sizeof(long))
                    warn_add("Failed to write bbs.sdata");
            } else {
                warn_add("Cannot open bbs.sdata for write");
            }

            ReleaseSemaphore(&myp->SEM[4]);
            ReleaseSemaphore(&myp->SEM[1]);
        }
    }

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---- maint repair-mail ---- */

/*
 * On-disk mail header: 810 bytes per record.
 * We operate on raw byte buffers (not the MailHeaderDisk struct from mail.c,
 * which is file-scope static). Field offsets used for compaction:
 *   +0:   unknown_0 (ULONG, big-endian) -- always zeroed
 *   +474: length    (USHORT, big-endian) -- body text byte count
 *   +800: seek      (ULONG, big-endian) -- offset into _mtext4
 */
#define MHEAD_RECORD_SIZE   810
#define MHEAD_OFF_UNKNOWN0    0
#define MHEAD_OFF_LENGTH    474
#define MHEAD_OFF_SEEK      800

/* Byte-order helpers (local to this compilation unit) */

static ULONG rm_read_be_ulong(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static USHORT rm_read_be_ushort(const UBYTE *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static void rm_write_be_ulong(UBYTE *p, ULONG val)
{
    p[0] = (UBYTE)(val >> 24);
    p[1] = (UBYTE)(val >> 16);
    p[2] = (UBYTE)(val >> 8);
    p[3] = (UBYTE)val;
}

static void rm_write_be_ushort(UBYTE *p, USHORT val)
{
    p[0] = (UBYTE)(val >> 8);
    p[1] = (UBYTE)val;
}

/*
 * Per-folder compaction result, used to aggregate stats.
 */
struct compact_result {
    int    records;
    long   old_text_size;
    long   new_text_size;
    long   bytes_reclaimed;
    int    bug3_fixed;
    int    error;           /* non-zero if folder was skipped on error */
};

/*
 * Build path to a file inside a mail folder.
 * folder_path is the output of CreateFolderName().
 */
static void build_mail_file_path(char *buf, int bufsz,
    const char *folder_path, const char *filename)
{
    int len = (int)strlen(folder_path);
    if (len > 0 && folder_path[len - 1] == '/')
        snprintf(buf, bufsz, "%s%s", folder_path, filename);
    else
        snprintf(buf, bufsz, "%s/%s", folder_path, filename);
}

/*
 * Compact one mail folder.
 *
 * Reads _mhead4 and _mtext4, rebuilds _mtext4 with dead text removed,
 * updates Seek offsets in headers, and zeros unknown_0.
 *
 * If dry_run is true, computes stats without writing.
 * Caller must hold the mail semaphore exclusive.
 *
 * Populates *result with compaction stats.
 */
static void compact_folder(const char *mhead_path,
    const char *mtext_path, int dry_run,
    struct compact_result *result)
{
    BPTR fh;
    long mhead_size, mtext_size;
    long n_records;
    UBYTE *mhead_buf = NULL;
    UBYTE *mtext_buf = NULL;
    UBYTE *new_mtext = NULL;
    ULONG new_offset;
    int i;
    char wbuf[128];

    memset(result, 0, sizeof(*result));

    /* Read _mhead4 */
    fh = Open((CONST_STRPTR)mhead_path, MODE_OLDFILE);
    if (!fh) {
        /* No _mhead4: check for orphaned _mtext4 */
        fh = Open((CONST_STRPTR)mtext_path, MODE_OLDFILE);
        if (fh) {
            Seek(fh, 0, OFFSET_END);
            mtext_size = Seek(fh, 0, OFFSET_BEGINNING);
            Close(fh);
            if (mtext_size > 0) {
                result->old_text_size = mtext_size;
                result->new_text_size = 0;
                result->bytes_reclaimed = mtext_size;
                if (!dry_run) {
                    /* Truncate orphaned _mtext4 */
                    fh = Open((CONST_STRPTR)mtext_path,
                        MODE_NEWFILE);
                    if (fh)
                        Close(fh);
                }
            }
        }
        return;
    }

    Seek(fh, 0, OFFSET_END);
    mhead_size = Seek(fh, 0, OFFSET_BEGINNING);

    if (mhead_size <= 0) {
        Close(fh);
        /* Empty _mhead4: check for orphaned _mtext4 */
        {
            BPTR fh2 = Open((CONST_STRPTR)mtext_path, MODE_OLDFILE);
            if (fh2) {
                Seek(fh2, 0, OFFSET_END);
                mtext_size = Seek(fh2, 0, OFFSET_BEGINNING);
                Close(fh2);
                if (mtext_size > 0) {
                    result->old_text_size = mtext_size;
                    result->new_text_size = 0;
                    result->bytes_reclaimed = mtext_size;
                    if (!dry_run) {
                        fh2 = Open((CONST_STRPTR)mtext_path,
                            MODE_NEWFILE);
                        if (fh2)
                            Close(fh2);
                    }
                }
            }
        }
        return;
    }

    if (mhead_size % MHEAD_RECORD_SIZE != 0) {
        Close(fh);
        snprintf(wbuf, sizeof(wbuf),
            "_mhead4 size %ld not multiple of %d",
            mhead_size, MHEAD_RECORD_SIZE);
        warn_add(wbuf);
        result->error = 1;
        return;
    }

    n_records = mhead_size / MHEAD_RECORD_SIZE;
    result->records = (int)n_records;

    mhead_buf = (UBYTE *)malloc((unsigned long)mhead_size);
    if (!mhead_buf) {
        Close(fh);
        warn_add("malloc failed for _mhead4");
        result->error = 1;
        return;
    }

    if (Read(fh, (APTR)mhead_buf, mhead_size) != mhead_size) {
        Close(fh);
        free(mhead_buf);
        warn_add("short read on _mhead4");
        result->error = 1;
        return;
    }
    Close(fh);

    /* Read _mtext4 */
    fh = Open((CONST_STRPTR)mtext_path, MODE_OLDFILE);
    if (fh) {
        Seek(fh, 0, OFFSET_END);
        mtext_size = Seek(fh, 0, OFFSET_BEGINNING);
        if (mtext_size > 0) {
            mtext_buf = (UBYTE *)malloc((unsigned long)mtext_size);
            if (!mtext_buf) {
                Close(fh);
                free(mhead_buf);
                warn_add("malloc failed for _mtext4");
                result->error = 1;
                return;
            }
            if (Read(fh, (APTR)mtext_buf, mtext_size) !=
                    mtext_size) {
                Close(fh);
                free(mtext_buf);
                free(mhead_buf);
                warn_add("short read on _mtext4");
                result->error = 1;
                return;
            }
        } else {
            mtext_size = 0;
        }
        Close(fh);
    } else {
        mtext_size = 0;
    }

    result->old_text_size = mtext_size;

    /* Allocate new text buffer (at most mtext_size bytes) */
    if (mtext_size > 0) {
        new_mtext = (UBYTE *)malloc((unsigned long)mtext_size);
        if (!new_mtext) {
            if (mtext_buf) free(mtext_buf);
            free(mhead_buf);
            warn_add("malloc failed for new _mtext4");
            result->error = 1;
            return;
        }
    }

    /* Rebuild text layout and update headers */
    new_offset = 0;
    for (i = 0; i < (int)n_records; i++) {
        UBYTE *rec = &mhead_buf[i * MHEAD_RECORD_SIZE];
        ULONG old_seek = rm_read_be_ulong(rec + MHEAD_OFF_SEEK);
        USHORT old_length = rm_read_be_ushort(rec + MHEAD_OFF_LENGTH);

        if (old_length > 0 && mtext_buf &&
                old_seek + old_length <= (ULONG)mtext_size) {
            memcpy(new_mtext + new_offset,
                mtext_buf + old_seek, old_length);
            rm_write_be_ulong(rec + MHEAD_OFF_SEEK, new_offset);
            new_offset += old_length;
        } else if (old_length == 0) {
            rm_write_be_ulong(rec + MHEAD_OFF_SEEK, 0);
        } else {
            /* Out of bounds -- clear text reference */
            snprintf(wbuf, sizeof(wbuf),
                "record %d: seek/length out of bounds, "
                "clearing text", i);
            warn_add(wbuf);
            rm_write_be_ushort(rec + MHEAD_OFF_LENGTH, 0);
            rm_write_be_ulong(rec + MHEAD_OFF_SEEK, 0);
        }

        /* Zero unknown_0 for all records (must always be zero) */
        if (rm_read_be_ulong(rec + MHEAD_OFF_UNKNOWN0) != 0) {
            result->bug3_fixed++;
        }
        memset(rec + MHEAD_OFF_UNKNOWN0, 0, 4);
    }

    result->new_text_size = (long)new_offset;
    result->bytes_reclaimed = mtext_size - (long)new_offset;

    /* Write files if applying and there is something to change */
    if (!dry_run && (result->bytes_reclaimed > 0 ||
            result->bug3_fixed > 0)) {
        char mtext_new[320], mhead_new[320];
        char mtext_old[320], mhead_old[320];
        int write_ok = 1;
        long nwritten;

        snprintf(mtext_new, sizeof(mtext_new),
            "%s.new", mtext_path);
        snprintf(mhead_new, sizeof(mhead_new),
            "%s.new", mhead_path);
        snprintf(mtext_old, sizeof(mtext_old),
            "%s.old", mtext_path);
        snprintf(mhead_old, sizeof(mhead_old),
            "%s.old", mhead_path);

        /* Write _mtext4.new */
        fh = Open((CONST_STRPTR)mtext_new, MODE_NEWFILE);
        if (!fh) {
            warn_add("cannot create _mtext4.new");
            write_ok = 0;
        } else {
            if (new_offset > 0) {
                nwritten = Write(fh, (APTR)new_mtext,
                    (long)new_offset);
                if (nwritten != (long)new_offset) {
                    warn_add("short write on _mtext4.new");
                    write_ok = 0;
                }
            }
            Close(fh);
        }

        /* Write _mhead4.new */
        if (write_ok) {
            fh = Open((CONST_STRPTR)mhead_new, MODE_NEWFILE);
            if (!fh) {
                warn_add("cannot create _mhead4.new");
                write_ok = 0;
            } else {
                nwritten = Write(fh, (APTR)mhead_buf,
                    mhead_size);
                if (nwritten != mhead_size) {
                    warn_add("short write on _mhead4.new");
                    write_ok = 0;
                }
                Close(fh);
            }
        }

        /* Atomic rename sequence:
         * _mtext4 first (see plan 5.4 rename order rationale) */
        if (write_ok) {
            /* Delete stale .old files that may exist from an
             * interrupted previous run */
            DeleteFile((CONST_STRPTR)mtext_old);
            DeleteFile((CONST_STRPTR)mhead_old);

            if (!Rename((CONST_STRPTR)mtext_path,
                    (CONST_STRPTR)mtext_old)) {
                warn_add("rename _mtext4 -> .old failed");
                write_ok = 0;
            }
        }
        if (write_ok) {
            if (!Rename((CONST_STRPTR)mtext_new,
                    (CONST_STRPTR)mtext_path)) {
                warn_add("rename _mtext4.new -> _mtext4 failed");
                /* Attempt recovery */
                Rename((CONST_STRPTR)mtext_old,
                    (CONST_STRPTR)mtext_path);
                write_ok = 0;
            }
        }
        if (write_ok) {
            if (!Rename((CONST_STRPTR)mhead_path,
                    (CONST_STRPTR)mhead_old)) {
                warn_add("rename _mhead4 -> .old failed");
                /* Roll back _mtext4: move new out, restore old */
                Rename((CONST_STRPTR)mtext_path,
                    (CONST_STRPTR)mtext_new);
                Rename((CONST_STRPTR)mtext_old,
                    (CONST_STRPTR)mtext_path);
                write_ok = 0;
            }
        }
        if (write_ok) {
            if (!Rename((CONST_STRPTR)mhead_new,
                    (CONST_STRPTR)mhead_path)) {
                warn_add("rename _mhead4.new -> _mhead4 failed");
                /* Attempt recovery of _mhead4 */
                Rename((CONST_STRPTR)mhead_old,
                    (CONST_STRPTR)mhead_path);
                /* Roll back _mtext4: move new out, restore old */
                Rename((CONST_STRPTR)mtext_path,
                    (CONST_STRPTR)mtext_new);
                Rename((CONST_STRPTR)mtext_old,
                    (CONST_STRPTR)mtext_path);
                write_ok = 0;
            }
        }

        /* Delete .old backups on success */
        if (write_ok) {
            DeleteFile((CONST_STRPTR)mtext_old);
            DeleteFile((CONST_STRPTR)mhead_old);
        }

        /* Clean up temp files on failure */
        if (!write_ok) {
            DeleteFile((CONST_STRPTR)mtext_new);
            DeleteFile((CONST_STRPTR)mhead_new);
            result->error = 1;
        }
    }

    if (new_mtext) free(new_mtext);
    if (mtext_buf) free(mtext_buf);
    free(mhead_buf);
}

/*
 * Resolve a UUCP name to an account number by scanning Key[].
 * Returns account number (1-based) or -1 if not found.
 * Caller must hold SEM[1] shared.
 */
static short uucp_to_account(struct MainPort *myp, const char *uucp)
{
    long total = myp->Nums[NUMS_CURRENT_ACCOUNTS];
    int i;

    for (i = 0; i < (int)total; i++) {
        if (myp->Key[i].Handle[0] != '\0' &&
                strcasecmp(myp->Key[i].UUCP, uucp) == 0)
            return (short)(i + 1);
    }
    return -1;
}

/*
 * Compact all folders for one user. Caller must hold the mail semaphore
 * exclusive for this account. Emits per-folder JSON into the folders
 * array.
 *
 * Returns: number of folders compacted (bytes_reclaimed > 0 or
 * bug3_fixed > 0).
 */
static int compact_user_folders(const char *uucp,
    const char *folder_name, int dry_run,
    struct json_state *js, long *total_bytes, int *folders_scanned)
{
    struct CNetFileEntry *dir_list;
    struct CNetFileEntry *entry;
    char folders_path[256];
    int compacted = 0;

    if (folder_name) {
        /* Single folder mode */
        char folder_path[256];
        char mhead_path[300];
        char mtext_path[300];
        struct compact_result cr;

        CreateFolderName(folder_path, (char *)uucp,
            (char *)folder_name);
        build_mail_file_path(mhead_path, (int)sizeof(mhead_path),
            folder_path, "_mhead4");
        build_mail_file_path(mtext_path, (int)sizeof(mtext_path),
            folder_path, "_mtext4");

        (*folders_scanned)++;
        compact_folder(mhead_path, mtext_path, dry_run, &cr);

        if (!cr.error && (cr.bytes_reclaimed > 0 ||
                cr.bug3_fixed > 0 || cr.records > 0)) {
            json_obj_open(js);
            json_kv_str(js, "name", folder_name);
            json_kv_int(js, "records", (long)cr.records);
            json_kv_int(js, "mtext4_old_size", cr.old_text_size);
            json_kv_int(js, "mtext4_new_size", cr.new_text_size);
            json_kv_int(js, "bytes_reclaimed", cr.bytes_reclaimed);
            json_kv_int(js, "bug3_records_fixed",
                (long)cr.bug3_fixed);
            json_obj_close(js);

            if (cr.bytes_reclaimed > 0 || cr.bug3_fixed > 0)
                compacted++;
            *total_bytes += cr.bytes_reclaimed;
        }
        return compacted;
    }

    /* Enumerate all folders */
    snprintf(folders_path, sizeof(folders_path),
        "mail:users/%s/FOLDERS", uucp);

    dir_list = CNetReadDir(folders_path, 0);
    if (!dir_list)
        return 0;

    /* Skip the directory header entry (first entry).
     * CNetReadDir returns the scanned directory itself as
     * the first entry. Actual contents start at nextfile. */
    entry = dir_list->nextfile;

    while (entry) {
        if (entry->ftype == CNFE_TYPE_DIR) {
            char folder_path[256];
            char mhead_path[300];
            char mtext_path[300];
            struct compact_result cr;

            CreateFolderName(folder_path, (char *)uucp,
                entry->filename);
            build_mail_file_path(mhead_path,
                (int)sizeof(mhead_path),
                folder_path, "_mhead4");
            build_mail_file_path(mtext_path,
                (int)sizeof(mtext_path),
                folder_path, "_mtext4");

            (*folders_scanned)++;
            compact_folder(mhead_path, mtext_path, dry_run, &cr);

            if (!cr.error && (cr.bytes_reclaimed > 0 ||
                    cr.bug3_fixed > 0 || cr.records > 0)) {
                json_obj_open(js);
                json_kv_str(js, "name", entry->filename);
                json_kv_int(js, "records", (long)cr.records);
                json_kv_int(js, "mtext4_old_size",
                    cr.old_text_size);
                json_kv_int(js, "mtext4_new_size",
                    cr.new_text_size);
                json_kv_int(js, "bytes_reclaimed",
                    cr.bytes_reclaimed);
                json_kv_int(js, "bug3_records_fixed",
                    (long)cr.bug3_fixed);
                json_obj_close(js);

                if (cr.bytes_reclaimed > 0 || cr.bug3_fixed > 0)
                    compacted++;
                *total_bytes += cr.bytes_reclaimed;
            }
        }

        entry = entry->nextfile;
    }

    CNetDisposeDir(&dir_list);
    return compacted;
}

/*
 * cmd_maint_repair_mail -- Compact mail data files.
 *
 * Modes:
 *   maint repair-mail <account|handle> [--folder <name>] [--apply]
 *   maint repair-mail --all [--apply]
 *
 * Default is dry-run (report only). --apply writes compacted files.
 *
 * Per-folder compaction removes dead text from _mtext4 and updates
 * Seek offsets in _mhead4. Also zeros unknown_0 in all records.
 *
 * Semaphore protocol: GetMailSems()[account-1] exclusive held per-user
 * across all that user's folders. SEM[1] shared for Key[] UUCP lookup.
 */
int cmd_maint_repair_mail(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int apply = 0;
    int all_users = 0;
    const char *user_arg = NULL;
    const char *folder_name = NULL;
    int i;
    int users_scanned = 0;
    int users_compacted = 0;
    int folders_scanned = 0;
    int total_folders_compacted = 0;
    long total_bytes_reclaimed = 0;
    struct SignalSemaphore *sems;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0) {
            apply = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            apply = 0;
        } else if (strcmp(argv[i], "--all") == 0) {
            all_users = 1;
        } else if (strcmp(argv[i], "--folder") == 0) {
            if (i + 1 >= argc) {
                json_error("--folder requires a folder name");
                return 1;
            }
            i++;
            folder_name = argv[i];
        } else if (argv[i][0] == '-') {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Unknown option: %s", argv[i]);
            json_error(buf);
            return 1;
        } else {
            /* Positional argument: user identifier */
            user_arg = argv[i];
        }
    }

    if (!user_arg && !all_users) {
        json_error("Usage: maint repair-mail <account|handle> "
            "[--folder <name>] [--apply]\n"
            "       maint repair-mail --all [--apply]");
        return 1;
    }
    if (user_arg && all_users) {
        json_error("Cannot specify both a user and --all");
        return 1;
    }
    if (folder_name && all_users) {
        json_error("--folder requires a specific user, "
            "not --all");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available "
            "(needed for mail semaphores)");
        return 1;
    }

    sems = GetMailSems();
    if (!sems) {
        json_error("mail semaphores not available");
        return 1;
    }

    warn_clear();

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "command", "maint_repair_mail");
    json_kv_str(&js, "mode", apply ? "apply" : "dry-run");

    json_key(&js, "users");
    json_arr_open(&js);

    if (user_arg) {
        /* Single-user mode */
        short account;
        char uucp[12];
        int fc;

        account = resolve_user_full(myp, user_arg, uucp,
            (int)sizeof(uucp));
        if (account < 1 || uucp[0] == '\0') {
            json_arr_close(&js);
            json_obj_close(&js);
            json_finish(&js);
            json_error("User not found");
            return 1;
        }

        users_scanned = 1;

        ObtainSemaphore(&sems[account - 1]);

        json_obj_open(&js);
        json_kv_str(&js, "uucp", uucp);
        json_kv_int(&js, "account", (long)account);

        json_key(&js, "folders");
        json_arr_open(&js);

        fc = compact_user_folders(uucp, folder_name, !apply,
            &js, &total_bytes_reclaimed, &folders_scanned);
        total_folders_compacted += fc;
        if (fc > 0)
            users_compacted++;

        json_arr_close(&js);
        json_obj_close(&js);

        ReleaseSemaphore(&sems[account - 1]);
    } else {
        /* All-users mode: enumerate mail:users/ */
        BPTR dir_lock;
        struct FileInfoBlock *fib;

        dir_lock = Lock((CONST_STRPTR)"mail:users/", ACCESS_READ);
        if (!dir_lock) {
            json_arr_close(&js);
            warn_add("cannot lock mail:users/ directory");
            goto finish;
        }

        fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
        if (!fib) {
            UnLock(dir_lock);
            json_arr_close(&js);
            warn_add("cannot allocate FileInfoBlock");
            goto finish;
        }

        if (!Examine(dir_lock, fib)) {
            FreeDosObject(DOS_FIB, fib);
            UnLock(dir_lock);
            json_arr_close(&js);
            warn_add("cannot examine mail:users/ directory");
            goto finish;
        }

        while (ExNext(dir_lock, fib)) {
            short account;
            int fc;
            char wbuf[128];

            /* Only process directories */
            if (fib->fib_DirEntryType <= 0)
                continue;

            /* Resolve UUCP name to account number */
            ObtainSemaphoreShared(&myp->SEM[1]);
            account = uucp_to_account(myp,
                (const char *)fib->fib_FileName);
            ReleaseSemaphore(&myp->SEM[1]);

            if (account < 1) {
                snprintf(wbuf, sizeof(wbuf),
                    "orphaned mail dir: %s (no matching account)",
                    (const char *)fib->fib_FileName);
                warn_add(wbuf);
                continue;
            }

            users_scanned++;

            ObtainSemaphore(&sems[account - 1]);

            json_obj_open(&js);
            json_kv_str(&js, "uucp",
                (const char *)fib->fib_FileName);
            json_kv_int(&js, "account", (long)account);

            json_key(&js, "folders");
            json_arr_open(&js);

            fc = compact_user_folders(
                (const char *)fib->fib_FileName, NULL,
                !apply, &js, &total_bytes_reclaimed,
                &folders_scanned);
            total_folders_compacted += fc;
            if (fc > 0)
                users_compacted++;

            json_arr_close(&js);
            json_obj_close(&js);

            ReleaseSemaphore(&sems[account - 1]);
        }

        FreeDosObject(DOS_FIB, fib);
        UnLock(dir_lock);
    }

    json_arr_close(&js);

finish:
    json_kv_int(&js, "users_scanned", (long)users_scanned);
    json_kv_int(&js, "users_compacted", (long)users_compacted);
    json_kv_int(&js, "folders_scanned", (long)folders_scanned);
    json_kv_int(&js, "folders_compacted",
        (long)total_folders_compacted);
    json_kv_int(&js, "total_bytes_reclaimed",
        total_bytes_reclaimed);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}

/* ---- maint repair-sub (not yet implemented) ---- */

/*
 * cmd_maint_repair_sub -- Subboard text pool compaction.
 *
 * DEFERRED: OneMoreUser() corrupts the libnix malloc heap, making all
 * subsequent malloc/realloc/free and stdio (fprintf/fputc) deadlock.
 * AllocMem/FreeMem bypass the libnix heap but the function still causes
 * Address Errors (80000003) from memory corruption. This operation
 * requires a different approach -- possibly direct file I/O without
 * OneMoreUser, or a native SAS/C-compiled helper.
 */
int cmd_maint_repair_sub(struct MainPort *myp, int argc, char **argv)
{
    (void)myp;
    (void)argc;
    (void)argv;

    json_error("maint repair-sub is not yet available "
        "(OneMoreUser heap corruption)");
    return 1;
}
