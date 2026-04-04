/*
 * subboard.c -- Subboard commands for cnet-cli
 *
 * Phase 2: sub list, sub show, sub tree
 * Phase 3: sub create, sub edit, sub delete
 *
 * Read commands acquire SEM[5] shared for safe concurrent access.
 * Mutation commands acquire SEM[5] exclusive and perform dual writes
 * (in-memory struct + SysData:subboards4 disk file).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "subboard.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/* ---- internal helpers ---- */

/*
 * Format an access field as "0x%08lx" into buf.
 * buf must be at least 11 bytes.
 */
static char *format_access(char *buf, int bufsz, unsigned long val)
{
    snprintf(buf, bufsz, "0x%08lx", val);
    return buf;
}

/*
 * Emit the common subboard summary fields used by both "sub list"
 * and the tree node output.
 */
static void emit_sub_summary(struct json_state *js,
    struct SubboardType4 *sub, int physnum, short root_sub)
{
    char buf[128];
    char abuf[16];
    int marker_base = sub->Marker & MRK_SUBBOARD_BASE;

    json_kv_int(js, "physnum", (long)physnum);
    json_kv_str(js, "title",
        strip_mci(buf, sizeof(buf), sub->Title));
    json_kv_str(js, "go_key",
        strip_mci(buf, sizeof(buf), sub->SubDirName));
    json_kv_int(js, "marker", (long)marker_base);
    json_kv_str(js, "marker_name", marker_type_name(marker_base));
    json_kv_bool(js, "killed",
        (sub->Marker & MRK_SUBBOARD_KILLED) ? 1 : 0);
    json_kv_bool(js, "root",
        (physnum == (int)root_sub) ? 1 : 0);
    json_kv_int(js, "parent", (long)sub->Parent);
    json_kv_int(js, "child", (long)sub->Child);
    json_kv_int(js, "next", (long)sub->Next);
    json_kv_str(js, "data_path",
        strip_mci(buf, sizeof(buf), sub->DataPath));
    json_kv_int(js, "users", (long)sub->Users);
    json_kv_uint(js, "items", (unsigned long)sub->count);
    json_kv_str(js, "access",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->Access));
    json_kv_uint(js, "serial", (unsigned long)sub->SerNum);
}

/*
 * Emit the full detail fields for "sub show" (everything beyond summary).
 */
static void emit_sub_detail(struct json_state *js,
    struct SubboardType4 *sub)
{
    char abuf[16];

    json_kv_uint(js, "item_count", (unsigned long)sub->count);
    json_kv_bool(js, "subdirectory", (int)sub->Subdirectory);
    json_kv_bool(js, "closed", (int)sub->Closed);
    json_kv_int(js, "max_items", (long)sub->MaxItems);
    json_kv_str(js, "post_access",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->PostAccess));
    json_kv_str(js, "respond_access",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->RespondAccess));
    json_kv_str(js, "upload_access",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->UploadAccess));
    json_kv_str(js, "download_access",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->DownloadAccess));
    json_kv_bool(js, "real_names", (int)sub->RealNames);
    json_kv_bool(js, "anonymous", (int)sub->Anonymous);
    json_kv_bool(js, "private_area", (int)sub->PrivateArea);
    json_kv_bool(js, "no_mci", (int)sub->NoMCI);
    json_kv_str(js, "computer_types",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->ComputerTypes));
    json_kv_int(js, "oldest", (long)sub->Oldest);
    json_kv_str(js, "arcs",
        format_access(abuf, sizeof(abuf), (unsigned long)sub->Arcs));
    json_kv_int(js, "subvalid", sub->subvalid);
}

/* ---- sub list ---- */

/*
 * Map --type argument string to marker base value(s).
 * Returns: 0 = match MsgBase only
 *          1 = match FileTxfer only
 *         -1 = match door types (3-9)
 *         -2 = invalid type string
 */
static int parse_type_filter(const char *arg)
{
    if (strcmp(arg, "msg") == 0) return 0;
    if (strcmp(arg, "file") == 0) return 1;
    if (strcmp(arg, "door") == 0) return -1;
    return -2;
}

/*
 * Check whether a subboard passes the type filter.
 * filter_type: 0=msg, 1=file, -1=door (3-9), -2=no filter
 */
static int passes_type_filter(int marker_base, int filter_type)
{
    if (filter_type == -2) return 1; /* no filter */
    if (filter_type == 0)  return marker_base == MRK_MSG_BASE;
    if (filter_type == 1)  return marker_base == MRK_FILE_TXFER;
    /* door: types 3-9 */
    return marker_base >= 3 && marker_base <= 9;
}

int cmd_sub_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int i;
    int active_only = 0;
    int filter_type = -2; /* no filter */

    /* Parse flags: --active, --type msg|file|door */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--active") == 0) {
            active_only = 1;
        } else if (strcmp(argv[i], "--type") == 0) {
            if (i + 1 >= argc) {
                json_error("--type requires an argument (msg, file, door)");
                return 1;
            }
            i++;
            filter_type = parse_type_filter(argv[i]);
            if (filter_type == -2) {
                json_error("Invalid --type value (use msg, file, or door)");
                return 1;
            }
        }
    }

    ObtainSemaphoreShared(&myp->SEM[5]);

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "subboards");
    json_arr_open(&js);

    for (i = 0; i < myp->ns; i++) {
        struct SubboardType4 *sub = &myp->Subboard[i];
        int marker_base = sub->Marker & MRK_SUBBOARD_BASE;
        int killed = (sub->Marker & MRK_SUBBOARD_KILLED) ? 1 : 0;

        /* Apply --active filter */
        if (active_only && killed)
            continue;

        /* Apply --type filter */
        if (!passes_type_filter(marker_base, filter_type))
            continue;

        json_obj_open(&js);
        emit_sub_summary(&js, sub, i, myp->root);
        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}

/* ---- sub show ---- */

int cmd_sub_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;

    if (argc < 2) {
        json_error("Usage: cnet-cli sub show <id|gokey>");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    ObtainSemaphoreShared(&myp->SEM[5]);

    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }

    sub = &myp->Subboard[physnum];

    json_init(&js, stdout);
    json_obj_open(&js);

    emit_sub_summary(&js, sub, (int)physnum, myp->root);
    emit_sub_detail(&js, sub);

    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}

/* ---- sub tree ---- */

/*
 * Bitmap for tracking visited subboard nodes during tree traversal.
 * Prevents infinite loops from corrupted Child/Next links.
 *
 * Maximum supported subboards: VISIT_MAP_BITS (4096).
 * The map is 512 bytes on the stack -- well within our 64KB budget.
 */
#define VISIT_MAP_BITS  4096
#define VISIT_MAP_WORDS ((VISIT_MAP_BITS + 31) / 32)

static void visit_clear(unsigned long *map)
{
    memset(map, 0, VISIT_MAP_WORDS * sizeof(unsigned long));
}

static int visit_test(const unsigned long *map, int n)
{
    if (n < 0 || n >= VISIT_MAP_BITS) return 1; /* out of range = visited */
    return (map[n / 32] >> (n % 32)) & 1;
}

static void visit_set(unsigned long *map, int n)
{
    if (n >= 0 && n < VISIT_MAP_BITS)
        map[n / 32] |= 1UL << (n % 32);
}

/*
 * Recursively emit a subtree rooted at physnum.
 * Follows Child link for nesting, Next link for siblings.
 * visited bitmap prevents infinite loops.
 */
static void emit_tree_node(struct json_state *js,
    struct MainPort *myp, int physnum, unsigned long *visited)
{
    struct SubboardType4 *sub;
    char buf[128];
    int marker_base;
    int child;

    /* Bounds and loop guard */
    if (physnum < 0 || physnum >= (int)myp->ns)
        return;
    if (visit_test(visited, physnum))
        return;
    visit_set(visited, physnum);

    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;

    json_obj_open(js);

    json_kv_int(js, "physnum", (long)physnum);
    json_kv_str(js, "title",
        strip_mci(buf, sizeof(buf), sub->Title));
    json_kv_str(js, "go_key",
        strip_mci(buf, sizeof(buf), sub->SubDirName));
    json_kv_str(js, "marker_name", marker_type_name(marker_base));
    json_kv_bool(js, "root",
        (physnum == (int)myp->root) ? 1 : 0);
    json_kv_bool(js, "subdirectory", (int)sub->Subdirectory);

    /* Children array (always present, may be empty) */
    json_key(js, "children");
    json_arr_open(js);

    child = (int)sub->Child;
    while (child >= 0 && child < (int)myp->ns) {
        if (visit_test(visited, child))
            break;
        emit_tree_node(js, myp, child, visited);
        /* After recursion, advance to sibling via Next */
        child = (int)myp->Subboard[child].Next;
    }

    json_arr_close(js);

    json_obj_close(js);
}

int cmd_sub_tree(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    unsigned long visited[VISIT_MAP_WORDS];

    (void)argc;
    (void)argv;

    ObtainSemaphoreShared(&myp->SEM[5]);

    visit_clear(visited);

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "tree");
    json_arr_open(&js);

    /*
     * Start from the root subboard. The tree is a single root
     * with children linked via Child/Next pointers.
     * If root has siblings at the top level (via Next), they are
     * also emitted as additional top-level entries.
     */
    if (myp->root >= 0 && myp->root < (short)myp->ns) {
        int node = (int)myp->root;
        while (node >= 0 && node < (int)myp->ns) {
            if (visit_test(visited, node))
                break;
            emit_tree_node(&js, myp, node, visited);
            node = (int)myp->Subboard[node].Next;
        }
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}

/* ---- sub path ---- */

/*
 * Walk the Parent chain from a subboard up to the root and emit
 * the ancestry path as a JSON array (leaf-to-root order).
 */
int cmd_sub_path(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    unsigned long visited[VISIT_MAP_WORDS];
    int depth = 0;
    int i;
    int cur;

    /*
     * Path entries collected during traversal.
     * Max depth 64 -- deeper hierarchies are pathological.
     * ~260 bytes per entry * 64 = ~16KB on the stack.
     */
    struct path_entry {
        int physnum;
        char title[128];
        char go_key[128];
    } path[64];

    if (argc < 2) {
        json_error("Usage: cnet-cli sub path <sub-id|gokey>");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    ObtainSemaphoreShared(&myp->SEM[5]);

    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }

    visit_clear(visited);

    /* Walk from the target up to the root */
    cur = (int)physnum;
    while (cur >= 0 && cur < (int)myp->ns && depth < 64) {
        struct SubboardType4 *sub;
        int parent;

        if (visit_test(visited, cur))
            break;
        visit_set(visited, cur);

        sub = &myp->Subboard[cur];

        path[depth].physnum = cur;
        strip_mci(path[depth].title, (int)sizeof(path[depth].title),
            sub->Title);
        strip_mci(path[depth].go_key, (int)sizeof(path[depth].go_key),
            sub->SubDirName);
        depth++;

        parent = (int)sub->Parent;

        /* Stop at root: Parent is self, negative, or out of range */
        if (parent == cur || parent < 0 || parent >= (int)myp->ns)
            break;

        cur = parent;
    }

    ReleaseSemaphore(&myp->SEM[5]);

    /* Emit JSON */
    json_init(&js, stdout);
    json_obj_open(&js);

    /* Top-level fields describe the queried subboard */
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_str(&js, "title", path[0].title);
    json_kv_str(&js, "go_key", path[0].go_key);
    json_kv_int(&js, "depth", (long)depth);

    json_key(&js, "path");
    json_arr_open(&js);

    for (i = 0; i < depth; i++) {
        json_obj_open(&js);
        json_kv_int(&js, "physnum", (long)path[i].physnum);
        json_kv_str(&js, "title", path[i].title);
        json_kv_str(&js, "go_key", path[i].go_key);
        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ==== Phase 3: Subboard mutation commands ==== */

/*
 * Disk file path for the subboard array.
 * This is a flat binary file of SubboardType4 records (696 bytes each).
 */
#define SUBBOARDS4_PATH "SysData:subboards4"

/*
 * Write a single SubboardType4 record to disk at the given physnum offset.
 * Must be called under SEM[5] exclusive lock.
 *
 * Returns 0 on success, -1 on I/O error.
 */
int write_subboard_disk(int physnum, struct SubboardType4 *sub)
{
    BPTR fh;
    long offset;
    long written;

    fh = Open((CONST_STRPTR)SUBBOARDS4_PATH, MODE_OLDFILE);
    if (!fh)
        return -1;

    offset = (long)physnum * (long)sizeof(struct SubboardType4);
    if (Seek(fh, offset, OFFSET_BEGINNING) == -1) {
        Close(fh);
        return -1;
    }

    written = Write(fh, (APTR)sub, (long)sizeof(struct SubboardType4));
    Close(fh);

    if (written != (long)sizeof(struct SubboardType4))
        return -1;

    return 0;
}

/*
 * Insert child_phys as the last child of parent_phys.
 * Walks the parent's Child->Next chain to find the tail.
 * Must be called under SEM[5] exclusive lock.
 */
static void insert_child(struct MainPort *myp, int parent_phys,
    int child_phys)
{
    struct SubboardType4 *parent = &myp->Subboard[parent_phys];
    struct SubboardType4 *child  = &myp->Subboard[child_phys];

    child->Parent = (short)parent_phys;
    child->Next   = -1;

    if (parent->Child < 0) {
        /* No existing children -- this is the first. */
        parent->Child = (short)child_phys;
    } else {
        /* Walk to end of sibling chain. */
        int cur = (int)parent->Child;
        int safety = 0;
        while (myp->Subboard[cur].Next >= 0 &&
               myp->Subboard[cur].Next < (short)myp->ns &&
               safety < VISIT_MAP_BITS) {
            cur = (int)myp->Subboard[cur].Next;
            safety++;
        }
        myp->Subboard[cur].Next = (short)child_phys;
    }
}

/*
 * Unlink child_phys from parent_phys's child chain.
 * Finds child_phys in the Parent->Child->Next linked list and
 * removes it, preserving the chain.
 * Must be called under SEM[5] exclusive lock.
 */
static void unlink_child(struct MainPort *myp, int parent_phys,
    int child_phys)
{
    struct SubboardType4 *parent = &myp->Subboard[parent_phys];

    if (parent->Child < 0)
        return;

    if ((int)parent->Child == child_phys) {
        /* First child -- promote its Next sibling. */
        parent->Child = myp->Subboard[child_phys].Next;
        return;
    }

    /* Walk the chain to find the predecessor. */
    {
        int prev = (int)parent->Child;
        int safety = 0;
        while (prev >= 0 && prev < (int)myp->ns &&
               safety < VISIT_MAP_BITS) {
            if ((int)myp->Subboard[prev].Next == child_phys) {
                myp->Subboard[prev].Next =
                    myp->Subboard[child_phys].Next;
                return;
            }
            prev = (int)myp->Subboard[prev].Next;
            safety++;
        }
    }
}

/*
 * Parse a --type argument into a Marker base value.
 * Returns the marker value (0-9) or -1 for invalid input.
 *
 * Supported types for creation:
 *   "msg"     -> 0 (MRK_MSG_BASE)
 *   "file"    -> 1 (MRK_FILE_TXFER)
 *   "subdir"  -> 0 (MRK_MSG_BASE with Subdirectory=1)
 *   "textdoor"-> 3 (MRK_TEXT_DOOR)
 *   "textfile"-> 4 (MRK_TEXT_FILE)
 *   "cdoor"   -> 5 (MRK_CNETC_DOOR)
 *   "arexx"   -> 6 (MRK_AREXX_DOOR)
 *   "ados"    -> 7 (MRK_ADOS_DOOR)
 *   "macro"   -> 8 (MRK_BBS_MACRO)
 *   "dircom"  -> 9 (MRK_DIRECT_COMMANDER)
 */
static int parse_marker_type_create(const char *arg, int *is_subdir)
{
    *is_subdir = 0;

    if (strcmp(arg, "msg") == 0)      return MRK_MSG_BASE;
    if (strcmp(arg, "file") == 0)     return MRK_FILE_TXFER;
    if (strcmp(arg, "subdir") == 0) {
        *is_subdir = 1;
        return MRK_MSG_BASE;
    }
    if (strcmp(arg, "textdoor") == 0) return MRK_TEXT_DOOR;
    if (strcmp(arg, "textfile") == 0) return MRK_TEXT_FILE;
    if (strcmp(arg, "cdoor") == 0)    return MRK_CNETC_DOOR;
    if (strcmp(arg, "arexx") == 0)    return MRK_AREXX_DOOR;
    if (strcmp(arg, "ados") == 0)     return MRK_ADOS_DOOR;
    if (strcmp(arg, "macro") == 0)    return MRK_BBS_MACRO;
    if (strcmp(arg, "dircom") == 0)   return MRK_DIRECT_COMMANDER;

    return -1;
}

/*
 * Parse a hexadecimal access string ("0xNNNNNNNN" or "NNNNNNNN").
 * Returns 1 on success (value stored in *out), 0 on failure.
 */
static int parse_hex_access(const char *s, unsigned long *out)
{
    char *endp;
    unsigned long val;

    if (!s || !*s)
        return 0;

    val = strtoul(s, &endp, 16);
    if (*endp != '\0')
        return 0;

    *out = val;
    return 1;
}

/* safe_strcpy is now a shared helper in util.c/util.h */

/*
 * Create an empty file at the given AmigaOS path.
 * Used to initialize MsgBase/FileTxfer data files.
 */
static void create_empty_file(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh) Close(fh);
}

/*
 * Initialize the required data files for a new MsgBase or FileTxfer
 * subboard.  CNet expects _Items3, _Headers3, and _Free to exist in
 * the data/ subdirectory; without them OneMoreUser() hangs.
 */
static void init_data_files(const char *data_path)
{
    char path[200];
    int dlen = strlen(data_path);
    const char *sep = (dlen > 0 && (data_path[dlen-1] == ':' ||
        data_path[dlen-1] == '/')) ? "" : "/";

    /* _Items3 and _Headers3: empty files */
    snprintf(path, sizeof(path), "%s%sdata/_Items3", data_path, sep);
    create_empty_file(path);

    snprintf(path, sizeof(path), "%s%sdata/_Headers3", data_path, sep);
    create_empty_file(path);

    /* _Free: 8 bytes of zeros (two big-endian longs = empty free list) */
    snprintf(path, sizeof(path), "%s%sdata/_Free", data_path, sep);
    {
        BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
        if (fh) {
            long zero[2] = {0, 0};
            Write(fh, (APTR)zero, 8);
            Close(fh);
        }
    }
}

/* ---- sub create ---- */

int cmd_sub_create(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    const char *title = NULL;
    const char *gokey = NULL;
    const char *type_str = NULL;
    const char *parent_str = NULL;
    const char *data_path = NULL;
    const char *access_str = NULL;
    const char *max_items_str = NULL;
    int marker_base;
    int is_subdir = 0;
    short parent_phys;
    int slot;
    int i;
    unsigned long access_val = 0;
    struct SubboardType4 *sub;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--title") == 0) {
            if (++i >= argc) {
                json_error("--title requires an argument");
                return 1;
            }
            title = argv[i];
        } else if (strcmp(argv[i], "--go") == 0) {
            if (++i >= argc) {
                json_error("--go requires an argument");
                return 1;
            }
            gokey = argv[i];
        } else if (strcmp(argv[i], "--type") == 0) {
            if (++i >= argc) {
                json_error("--type requires an argument");
                return 1;
            }
            type_str = argv[i];
        } else if (strcmp(argv[i], "--parent") == 0) {
            if (++i >= argc) {
                json_error("--parent requires an argument");
                return 1;
            }
            parent_str = argv[i];
        } else if (strcmp(argv[i], "--data-path") == 0) {
            if (++i >= argc) {
                json_error("--data-path requires an argument");
                return 1;
            }
            data_path = argv[i];
        } else if (strcmp(argv[i], "--access") == 0) {
            if (++i >= argc) {
                json_error("--access requires an argument");
                return 1;
            }
            access_str = argv[i];
        } else if (strcmp(argv[i], "--max-items") == 0) {
            if (++i >= argc) {
                json_error("--max-items requires an argument");
                return 1;
            }
            max_items_str = argv[i];
        }
    }

    /* Validate required arguments */
    if (!title || !gokey || !type_str || !parent_str) {
        json_error("Usage: sub create --title <t> --go <key> "
            "--type <type> --parent <id|gokey> "
            "[--data-path <path>] [--access <hex>] [--max-items N]");
        return 1;
    }

    /* Parse type */
    marker_base = parse_marker_type_create(type_str, &is_subdir);
    if (marker_base < 0) {
        json_error("Invalid --type (use msg, file, subdir, "
            "textdoor, cdoor, arexx, ados, macro, dircom)");
        return 1;
    }

    /* Parse optional access */
    if (access_str) {
        if (!parse_hex_access(access_str, &access_val)) {
            json_error("Invalid --access (use hex, e.g. 0xFFFFFFFF)");
            return 1;
        }
    }

    /*
     * Resolve parent BEFORE acquiring exclusive lock.
     * resolve_subboard() acquires SEM[5] shared internally.
     */
    parent_phys = resolve_subboard(myp, parent_str);
    if (parent_phys < 0) {
        json_error("Parent subboard not found");
        return 1;
    }

    /* Acquire SEM[5] exclusive for the entire mutation. */
    ObtainSemaphore(&myp->SEM[5]);

    /* Verify parent is still valid under lock. */
    if (parent_phys >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Parent subboard out of range");
        return 1;
    }

    /*
     * Find a free slot: scan for a killed entry first,
     * then extend if allowed.
     */
    slot = -1;
    for (i = 0; i < (int)myp->ns; i++) {
        if (myp->Subboard[i].Marker & MRK_SUBBOARD_KILLED) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /*
         * No killed slot -- try to extend.
         * CNet Control pre-allocates gc.nsub SubboardType4 entries
         * at boot, so extending ns up to gc.nsub is safe without
         * reallocation.
         */
        if (myp->ns >= myp->gc.nsub) {
            ReleaseSemaphore(&myp->SEM[5]);
            json_error("Maximum subboard count reached");
            return 1;
        }
        slot = (int)myp->ns;
        myp->ns++;
    }

    /* Initialize the SubboardType4 struct.
     * Preserve sub->sem: CNet Control allocates and initializes a
     * SignalSemaphore for each subboard slot at boot. OneMoreUser and
     * OneLessUser call ObtainSemaphore(sub->sem) early -- if sem is
     * NULL (from memset), ObtainSemaphore hangs forever on 68k. */
    sub = &myp->Subboard[slot];
    {
        struct SignalSemaphore *saved_sem = sub->sem;
        memset(sub, 0, sizeof(struct SubboardType4));
        sub->sem = saved_sem;
    }

    /* Access-control defaults matching Config GUI behavior.
     * Zero values block all access (no computer types, max age 0,
     * no arcs). */
    sub->ComputerTypes = (long)0xFFFFFFFF;  /* all computer types allowed */
    sub->Oldest        = 99;          /* max age 99 years */
    sub->Arcs          = (long)0xFFFFFFFF;  /* all archive types allowed */

    safe_strcpy(sub->Title, title, (int)sizeof(sub->Title));
    safe_strcpy(sub->SubDirName, gokey, (int)sizeof(sub->SubDirName));

    if (data_path) {
        safe_strcpy(sub->DataPath, data_path,
            (int)sizeof(sub->DataPath));
    } else {
        /*
         * Derive DataPath from parent's DataPath + GO key + "/".
         * AmigaOS path joining: "Volume:" + name needs no separator,
         * "dir/" + name needs "/" separator.
         */
        struct SubboardType4 *par = &myp->Subboard[parent_phys];
        int plen = (int)strlen(par->DataPath);
        if (plen > 0 &&
            plen + (int)strlen(gokey) + 2 < (int)sizeof(sub->DataPath)) {
            if (par->DataPath[plen - 1] == ':' ||
                par->DataPath[plen - 1] == '/') {
                snprintf(sub->DataPath, sizeof(sub->DataPath),
                    "%s%s/", par->DataPath, gokey);
            } else {
                snprintf(sub->DataPath, sizeof(sub->DataPath),
                    "%s/%s/", par->DataPath, gokey);
            }
        }
    }

    /* Marker: type + not killed */
    sub->Marker = (UBYTE)marker_base;
    sub->Subdirectory = (UBYTE)is_subdir;

    /* Tree linkage (set before insert_child, which overwrites some) */
    sub->Child = -1;
    sub->Next  = -1;

    /* Access fields */
    if (access_str) {
        sub->Access        = (long)access_val;
        sub->PostAccess    = (long)access_val;
        sub->RespondAccess = (long)access_val;
        sub->UploadAccess  = (long)access_val;
        sub->DownloadAccess = (long)access_val;
    } else {
        sub->Access        = (long)0xFFFFFFFF;
        sub->PostAccess    = (long)0xFFFFFFFF;
        sub->RespondAccess = (long)0xFFFFFFFF;
        sub->UploadAccess  = (long)0xFFFFFFFF;
        sub->DownloadAccess = (long)0xFFFFFFFF;
    }

    /* MaxItems */
    if (max_items_str) {
        long mi = atol(max_items_str);
        if (mi > 0 && mi <= HIGH_ITEM_LIMIT)
            sub->MaxItems = (USHORT)mi;
        else
            sub->MaxItems = 500;
    } else {
        sub->MaxItems = 500;
    }

    /* Validity sentinel */
    sub->subvalid = 1234567890L;

    /*
     * Serial number from global config.
     *
     * NOTE: nextsubser is incremented in memory only. CNet Control
     * saves the config periodically and at shutdown. If the system
     * crashes before Control saves, a duplicate SerNum could be
     * assigned on the next create. This matches CNet's own behavior
     * for subboard creation via the Config GUI.
     */
    if (myp->MPE) {
        sub->SerNum = myp->MPE->gc2.nextsubser;
        myp->MPE->gc2.nextsubser++;
    }

    /* Insert into parent's child list. */
    insert_child(myp, (int)parent_phys, slot);

    /* Dual write: disk persistence for both the new sub and the parent. */
    if (write_subboard_disk(slot, sub) != 0) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Failed to write new subboard to disk");
        return 1;
    }
    /* Parent's Child or a sibling's Next was updated by insert_child. */
    if (write_subboard_disk((int)parent_phys,
            &myp->Subboard[parent_phys]) != 0) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Failed to write parent subboard to disk");
        return 1;
    }
    /*
     * If insert_child appended to an existing sibling (not the parent's
     * Child directly), that sibling's Next was also modified. We need
     * to find and write it. Walk the chain to find the predecessor.
     */
    if (myp->Subboard[parent_phys].Child != (short)slot) {
        int prev = (int)myp->Subboard[parent_phys].Child;
        int safety = 0;
        while (prev >= 0 && prev < (int)myp->ns &&
               safety < VISIT_MAP_BITS) {
            if ((int)myp->Subboard[prev].Next == slot) {
                write_subboard_disk(prev, &myp->Subboard[prev]);
                break;
            }
            prev = (int)myp->Subboard[prev].Next;
            safety++;
        }
    }

    /* S2: Create the data directory for the new subboard. */
    if (sub->DataPath[0] && CNetBase) {
        char dir_buf[200];
        int dlen = strlen(sub->DataPath);
        /* Build path to data/ subdir using AmigaOS path joining */
        if (dlen > 0 && (sub->DataPath[dlen-1] == ':' || sub->DataPath[dlen-1] == '/'))
            snprintf(dir_buf, sizeof(dir_buf), "%sdata", sub->DataPath);
        else
            snprintf(dir_buf, sizeof(dir_buf), "%s/data", sub->DataPath);
        BuildDir((char *)dir_buf);

        /* Initialize empty data files for MsgBase and FileTxfer types */
        if ((sub->Marker & MRK_SUBBOARD_BASE) <= MRK_FILE_TXFER) {
            init_data_files(sub->DataPath);
        }
    }

    /* Output JSON result while still holding SEM[5]. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "created");
    json_kv_int(&js, "physnum", (long)slot);
    emit_sub_summary(&js, sub, slot, myp->root);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}

/* ---- sub edit ---- */

int cmd_sub_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    int i;
    int changed = 0;

    /* First positional argument is the subboard id/gokey. */
    if (argc < 2) {
        json_error("Usage: sub edit <id|gokey> [--title <t>] "
            "[--go <key>] [--type <type>] [--data-path <path>] "
            "[--access <hex>] [--post-access <hex>] "
            "[--respond-access <hex>] [--upload-access <hex>] "
            "[--download-access <hex>] [--max-items N] "
            "[--closed true|false] [--real-names true|false] "
            "[--anonymous true|false] [--private true|false] "
            "[--no-mci true|false] "
            "[--computer-types <hex>] [--oldest N] [--arcs <hex>]");
        return 1;
    }

    /*
     * Resolve subboard BEFORE acquiring exclusive lock.
     * resolve_subboard() acquires SEM[5] shared internally.
     */
    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    /* Acquire SEM[5] exclusive. */
    ObtainSemaphore(&myp->SEM[5]);

    /* Verify slot is still valid under lock. */
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }

    sub = &myp->Subboard[physnum];

    /* Apply field modifications. */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            i++;
            safe_strcpy(sub->Title, argv[i],
                (int)sizeof(sub->Title));
            changed = 1;
        } else if (strcmp(argv[i], "--go") == 0 && i + 1 < argc) {
            i++;
            safe_strcpy(sub->SubDirName, argv[i],
                (int)sizeof(sub->SubDirName));
            changed = 1;
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            int is_sd = 0;
            int mb;
            i++;
            mb = parse_marker_type_create(argv[i], &is_sd);
            if (mb < 0) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --type value");
                return 1;
            }
            /* Preserve killed bit, replace base type. */
            sub->Marker = (UBYTE)((sub->Marker & MRK_SUBBOARD_KILLED)
                | (UBYTE)mb);
            sub->Subdirectory = (UBYTE)is_sd;
            changed = 1;
        } else if (strcmp(argv[i], "--data-path") == 0 &&
                   i + 1 < argc) {
            i++;
            safe_strcpy(sub->DataPath, argv[i],
                (int)sizeof(sub->DataPath));
            changed = 1;
        } else if (strcmp(argv[i], "--access") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --access hex value");
                return 1;
            }
            sub->Access = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--post-access") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --post-access hex value");
                return 1;
            }
            sub->PostAccess = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--respond-access") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --respond-access hex value");
                return 1;
            }
            sub->RespondAccess = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--upload-access") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --upload-access hex value");
                return 1;
            }
            sub->UploadAccess = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--download-access") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --download-access hex value");
                return 1;
            }
            sub->DownloadAccess = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--max-items") == 0 &&
                   i + 1 < argc) {
            long mi;
            i++;
            mi = atol(argv[i]);
            if (mi > 0 && mi <= HIGH_ITEM_LIMIT) {
                sub->MaxItems = (USHORT)mi;
                changed = 1;
            } else {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --max-items value (must be 1-4000)");
                return 1;
            }
        } else if (strcmp(argv[i], "--closed") == 0 &&
                   i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "true") == 0)
                sub->Closed = 1;
            else
                sub->Closed = 0;
            changed = 1;
        } else if (strcmp(argv[i], "--real-names") == 0 &&
                   i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "true") == 0)
                sub->RealNames = 1;
            else
                sub->RealNames = 0;
            changed = 1;
        } else if (strcmp(argv[i], "--anonymous") == 0 &&
                   i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "true") == 0)
                sub->Anonymous = 1;
            else
                sub->Anonymous = 0;
            changed = 1;
        } else if (strcmp(argv[i], "--private") == 0 &&
                   i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "true") == 0)
                sub->PrivateArea = 1;
            else
                sub->PrivateArea = 0;
            changed = 1;
        } else if (strcmp(argv[i], "--no-mci") == 0 &&
                   i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "true") == 0)
                sub->NoMCI = 1;
            else
                sub->NoMCI = 0;
            changed = 1;
        } else if (strcmp(argv[i], "--computer-types") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --computer-types hex value");
                return 1;
            }
            sub->ComputerTypes = (long)v;
            changed = 1;
        } else if (strcmp(argv[i], "--oldest") == 0 &&
                   i + 1 < argc) {
            long v;
            i++;
            v = atol(argv[i]);
            if (v < 0 || v > 255) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --oldest value (must be 0-255)");
                return 1;
            }
            sub->Oldest = (UBYTE)v;
            changed = 1;
        } else if (strcmp(argv[i], "--arcs") == 0 &&
                   i + 1 < argc) {
            unsigned long v;
            i++;
            if (!parse_hex_access(argv[i], &v)) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Invalid --arcs hex value");
                return 1;
            }
            sub->Arcs = (long)v;
            changed = 1;
        }
    }

    if (!changed) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("No fields modified (use --title, --go, etc.)");
        return 1;
    }

    /* Dual write to disk. */
    if (write_subboard_disk((int)physnum, sub) != 0) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Failed to write subboard to disk");
        return 1;
    }

    /* Output JSON result while still holding SEM[5]. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "updated");
    emit_sub_summary(&js, sub, (int)physnum, myp->root);
    emit_sub_detail(&js, sub);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}

/* ---- sub delete ---- */

int cmd_sub_delete(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    short physnum;
    struct SubboardType4 *sub;
    int force = 0;
    int i;
    int parent_phys;
    int has_children;

    if (argc < 2) {
        json_error("Usage: sub delete <id|gokey> [--force]");
        return 1;
    }

    /* Check for --force flag. */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0)
            force = 1;
    }

    /*
     * Resolve subboard BEFORE acquiring exclusive lock.
     * resolve_subboard() acquires SEM[5] shared internally.
     */
    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    /* Refuse to delete the root subboard. */
    if (physnum == myp->root) {
        json_error("Cannot delete the root subboard");
        return 1;
    }

    /* Acquire SEM[5] exclusive. */
    ObtainSemaphore(&myp->SEM[5]);

    /* Verify slot is still valid under lock. */
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }

    sub = &myp->Subboard[physnum];

    /* Already killed? */
    if (sub->Marker & MRK_SUBBOARD_KILLED) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is already deleted");
        return 1;
    }

    /* Check for children. */
    has_children = (sub->Child >= 0 &&
                    sub->Child < (short)myp->ns);

    if (has_children && !force) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard has children; use --force to "
            "reparent them and delete");
        return 1;
    }

    parent_phys = (int)sub->Parent;

    /* Validate parent_phys before using it for tree operations. */
    if (parent_phys < 0 || parent_phys >= (int)myp->ns) {
        /*
         * Orphaned subboard -- no valid parent to reparent children
         * to or unlink from. Just mark as killed, clear links, write
         * to disk, and skip all tree fixup.
         */
        sub->Marker |= MRK_SUBBOARD_KILLED;
        sub->Child = -1;
        sub->Next  = -1;

        if (write_subboard_disk((int)physnum, sub) != 0) {
            ReleaseSemaphore(&myp->SEM[5]);
            json_error("Failed to write deleted subboard to disk");
            return 1;
        }

        /* Output JSON result while still holding SEM[5]. */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_kv_str(&js, "status", "deleted");
        json_kv_int(&js, "physnum", (long)physnum);
        json_kv_str(&js, "title",
            strip_mci(buf, sizeof(buf), sub->Title));
        json_obj_close(&js);
        json_finish(&js);

        ReleaseSemaphore(&myp->SEM[5]);
        return 0;
    }

    /*
     * If --force and has children: reparent all children to
     * this subboard's parent.
     */
    if (has_children && force) {
        int child = (int)sub->Child;
        int last_child_phys;
        int safety = 0;

        while (child >= 0 && child < (int)myp->ns &&
               safety < VISIT_MAP_BITS) {
            int next_child = (int)myp->Subboard[child].Next;

            myp->Subboard[child].Parent = (short)parent_phys;

            /*
             * For the last child in the chain, link it into
             * the parent's child list by finding the tail.
             */
            if (next_child < 0 || next_child >= (int)myp->ns) {
                /*
                 * This is the last child. Splice the entire
                 * reparented chain into the parent.
                 * The chain starts at sub->Child and ends here.
                 */
                break;
            }

            child = next_child;
            safety++;
        }

        /* child is now the last node in the reparented chain. */
        last_child_phys = child;

        /*
         * Insert the chain of reparented children into the parent.
         * Link the last reparented child's Next to whatever was
         * after the deleted node in the parent's child list, then
         * splice the chain head where the deleted node was.
         *
         * Strategy:
         * 1. Record the deleted node's Next sibling.
         * 2. The reparented chain is sub->Child through last_child_phys.
         * 3. Link last_child_phys->Next = deleted node's Next sibling.
         * 4. Replace the deleted node in parent's chain with the
         *    reparented chain head.
         */
        {
            short deleted_next = sub->Next;
            short chain_head = sub->Child;

            /* Link end of reparented chain to deleted node's sibling */
            myp->Subboard[last_child_phys].Next = deleted_next;

            /* Replace deleted node in parent's child list */
            if ((int)myp->Subboard[parent_phys].Child == (int)physnum) {
                myp->Subboard[parent_phys].Child = chain_head;
            } else {
                int prev = (int)myp->Subboard[parent_phys].Child;
                int s2 = 0;
                while (prev >= 0 && prev < (int)myp->ns &&
                       s2 < VISIT_MAP_BITS) {
                    if ((int)myp->Subboard[prev].Next == (int)physnum) {
                        myp->Subboard[prev].Next = chain_head;
                        break;
                    }
                    prev = (int)myp->Subboard[prev].Next;
                    s2++;
                }
            }
        }

        /*
         * Write all reparented children to disk. Stop after writing
         * the last reparented child to avoid walking into unrelated
         * siblings that follow via the Next link.
         */
        {
            int rc_child = (int)sub->Child;
            int s3 = 0;
            while (rc_child >= 0 && rc_child < (int)myp->ns &&
                   s3 < VISIT_MAP_BITS) {
                int next_rc = (int)myp->Subboard[rc_child].Next;
                write_subboard_disk(rc_child,
                    &myp->Subboard[rc_child]);
                if (rc_child == last_child_phys)
                    break;
                rc_child = next_rc;
                s3++;
            }
        }
    } else {
        /*
         * No children (or already empty): simple unlink from
         * parent's child chain.
         */
        if (parent_phys >= 0 && parent_phys < (int)myp->ns)
            unlink_child(myp, parent_phys, (int)physnum);
    }

    /* Mark as killed. */
    sub->Marker |= MRK_SUBBOARD_KILLED;
    sub->Child = -1;
    sub->Next  = -1;

    /* Dual write: the deleted subboard itself. */
    if (write_subboard_disk((int)physnum, sub) != 0) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Failed to write deleted subboard to disk");
        return 1;
    }

    /* Write the parent (its Child or a sibling's Next changed). */
    if (parent_phys >= 0 && parent_phys < (int)myp->ns) {
        write_subboard_disk(parent_phys,
            &myp->Subboard[parent_phys]);

        /*
         * If unlink_child modified a sibling's Next (not the parent's
         * Child directly), we need to write that sibling too.
         * Walk the parent's child chain to find any node that might
         * have been the predecessor. Since we already unlinked, the
         * predecessor now points past us -- but we wrote it during
         * unlink or reparent above. For safety, write all siblings
         * that could have been modified. In practice, unlink_child
         * only changes one node, and the reparent block above writes
         * all reparented children. The predecessor sibling may need
         * writing though.
         */
        {
            int sib = (int)myp->Subboard[parent_phys].Child;
            int s4 = 0;
            while (sib >= 0 && sib < (int)myp->ns &&
                   s4 < VISIT_MAP_BITS) {
                /*
                 * Write every sibling that could have been the
                 * modified predecessor. This is slightly over-broad
                 * but safe and bounded.
                 */
                write_subboard_disk(sib, &myp->Subboard[sib]);
                sib = (int)myp->Subboard[sib].Next;
                s4++;
            }
        }
    }

    /* Output JSON result while still holding SEM[5]. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "deleted");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), sub->Title));
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[5]);
    return 0;
}
