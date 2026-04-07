/*
 * bbslist.c -- BBSList commands for cnet-cli
 *
 * BBS directory listing. Reads sysdata:bbslist/bbslist as sequential
 * BBSItem records. No semaphore needed (no dedicated BBSList semaphore
 * in MainPort).
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#include <cnet/bbslist.h>
#include <cnet/sysfiles.h>
#include <cnet/dates.h>
#undef __asm

#include <proto/dos.h>

#include "bbslist.h"
#include "json.h"
#include "util.h"

/*
 * Compile-time verification that BBSItem matches the on-disk format.
 * Size confirmed via m68k-amigaos-gcc cross-compiler against SDK header.
 */
_Static_assert(sizeof(struct BBSItem) == 158,
    "BBSItem must be 158 bytes (SDK bbslist.h with m68k alignment)");

int cmd_bbslist_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct BBSItem item;
    BPTR fh;
    long fsize;
    long total_records;
    int index = 0;
    int emitted_count = 0;
    int show_all = 0;
    int i;
    char date_buf[24];

    (void)myp;

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0)
            show_all = 1;
    }

    fh = Open((CONST_STRPTR)BBSLISTNAME, MODE_OLDFILE);
    if (!fh) {
        /* No BBSList file -- emit empty result */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_key(&js, "entries");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_kv_int(&js, "total_records", 0);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    /* Determine file size and total record count */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    total_records = fsize / (long)sizeof(struct BBSItem);

    if (fsize % (long)sizeof(struct BBSItem) != 0) {
        warn_add("BBSList file size is not a multiple of record size"
            " (possible B-tree index data)");
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "entries");
    json_arr_open(&js);

    while (Read(fh, &item, sizeof(struct BBSItem))
            == (long)sizeof(struct BBSItem)) {
        if (!show_all && item.killed) {
            index++;
            continue;
        }

        json_obj_open(&js);
        json_kv_int(&js, "index", (long)index);
        json_kv_int(&js, "user_id", item.ID);
        json_kv_str(&js, "phone", item.Phone);
        json_kv_str(&js, "title", item.Title);
        json_kv_str(&js, "location", item.Location);
        json_kv_str(&js, "baud", item.Baud);
        json_kv_str(&js, "comments", item.Comments);
        json_kv_str(&js, "country", item.Country);
        json_kv_str(&js, "flags", item.Flags);
        json_kv_bool(&js, "immortal", (int)item.Immortal);

        if (is_null_date(&item.Date))
            json_kv_null(&js, "date");
        else
            json_kv_str(&js, "date",
                format_date(date_buf, sizeof(date_buf), &item.Date));

        if (show_all)
            json_kv_bool(&js, "killed", (int)item.killed);

        json_obj_close(&js);

        emitted_count++;
        index++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "count", (long)emitted_count);
    json_kv_int(&js, "total_records", (long)total_records);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    Close(fh);
    return 0;
}
