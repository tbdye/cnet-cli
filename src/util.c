/*
 * util.c -- String utilities for cnet-cli
 */

#include "util.h"
#include "json.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <exec/types.h>

/*
 * CNet SDK master include.
 * Pulls in all CNet types (IsDate, etc.), and for __GNUC__ the
 * proto/cnet.h -> inline/cnet.h chain gives us the MCIRemove stub.
 */
#include <cnet/cnet.h>

/*
 * cnet.h redefines __asm to nothing for SAS/C compatibility.
 * Undo it before including AmigaOS proto headers, which need
 * __asm as a GCC keyword for __REG() register macros.
 */
#undef __asm

#include <proto/exec.h>
#include <proto/cnet4.h>

/*
 * CNetBase is declared extern in proto/cnet.h. We reference it here
 * so we can conditionally call MCIRemove. The actual definition is
 * in main.c.
 */
extern struct Library *CNetBase;

/* From main.c -- may be NULL if cnet4.library is not available */
extern struct Library *CNet4Base;

/*
 * Build a path to a file under a subboard's data/ directory.
 * Handles AmigaOS path joining: volume: needs no separator,
 * directory/ needs no extra separator.
 */
void build_data_file_path(char *buf, int bufsz,
    const char *data_path, const char *filename)
{
    int len = (int)strlen(data_path);

    if (len > 0 && (data_path[len - 1] == ':' ||
                     data_path[len - 1] == '/')) {
        snprintf(buf, bufsz, "%sdata/%s", data_path, filename);
    } else {
        snprintf(buf, bufsz, "%s/data/%s", data_path, filename);
    }
}

char *strip_mci(char *buf, int bufsz, const char *src)
{
    if (!src) {
        buf[0] = '\0';
        return buf;
    }

    strncpy(buf, src, bufsz - 1);
    buf[bufsz - 1] = '\0';

    if (CNetBase) {
        MCIRemove(buf);
    }

    return buf;
}

char *format_date(char *buf, int bufsz, const void *date_ptr)
{
    const struct IsDate *d = (const struct IsDate *)date_ptr;

    if (bufsz < 20) {
        buf[0] = '\0';
        return buf;
    }

    sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
        (int)d->Year + ISDATE_BASE_YEAR,
        (int)d->Month,
        (int)d->Date,
        (int)d->Hour,
        (int)d->Minute,
        (int)d->Second);

    return buf;
}

int is_null_date(const void *date_ptr)
{
    const unsigned char *p = (const unsigned char *)date_ptr;
    int i;

    for (i = 0; i < 6; i++) {
        if (p[i] != 0) return 0;
    }
    return 1;
}

const char *marker_type_name(int marker_base)
{
    switch (marker_base) {
    case 0: return "MsgBase";
    case 1: return "FileTxfer";
    case 3: return "TextDoor";
    case 4: return "TextFile";
    case 5: return "CNetCDoor";
    case 6: return "ARexxDoor";
    case 7: return "ADosDoor";
    case 8: return "BBSMacro";
    case 9: return "DirCommander";
    default: return "Unknown";
    }
}

/*
 * Check if a string is all decimal digits.
 * Returns 1 if numeric, 0 otherwise. Empty string returns 0.
 */
int all_digits(const char *s)
{
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

/*
 * Case-insensitive substring search.
 * Returns 1 if needle is found within haystack, 0 otherwise.
 * Empty needle matches everything.
 */
int ci_contains(const char *haystack, const char *needle)
{
    int nlen, hlen;
    int i, j;

    if (!haystack || !needle) return 0;

    nlen = (int)strlen(needle);
    hlen = (int)strlen(haystack);

    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;

    for (i = 0; i <= hlen - nlen; i++) {
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/*
 * Safe string copy for fixed-size char arrays.
 * Always null-terminates within maxlen bytes.
 */
void safe_strcpy(char *dest, const char *src, int maxlen)
{
    strncpy(dest, src, maxlen - 1);
    dest[maxlen - 1] = '\0';
}

short resolve_user_full(struct MainPort *myp, const char *id_or_handle,
    char *out_uucp, int uucp_bufsz)
{
    if (!id_or_handle || !*id_or_handle)
        return -1;

    /* Try numeric parse first */
    if (all_digits(id_or_handle)) {
        long val = atol(id_or_handle);
        if (val >= 1 && val <= myp->Nums[NUMS_CURRENT_ACCOUNTS]) {
            if (out_uucp && uucp_bufsz > 0) {
                ObtainSemaphoreShared(&myp->SEM[1]);
                strncpy(out_uucp,
                    myp->Key[(int)val - 1].UUCP,
                    uucp_bufsz - 1);
                out_uucp[uucp_bufsz - 1] = '\0';
                ReleaseSemaphore(&myp->SEM[1]);
            }
            return (short)val;
        }
        return -1;
    }

    /* Handle lookup via FindHandle from cnet.library.
     * FindHandle returns a sorted index into IName[], not an account
     * number.  Map through myp->IName[n] to get the 1-based account.
     * IName[] is protected by SEM[1].
     * FindHandle third param (id) = 0 means "don't skip any account".
     */
    if (CNetBase) {
        short n = 0;
        long found = FindHandle(&n, (char *)id_or_handle, 0);
        if (found && n >= 0) {
            short account;
            ObtainSemaphoreShared(&myp->SEM[1]);
            account = myp->IName[n];
            if (out_uucp && uucp_bufsz > 0) {
                strncpy(out_uucp,
                    myp->Key[account - 1].UUCP,
                    uucp_bufsz - 1);
                out_uucp[uucp_bufsz - 1] = '\0';
            }
            ReleaseSemaphore(&myp->SEM[1]);
            return account;
        }
    }

    return -1;
}

short resolve_subboard(struct MainPort *myp, const char *id_or_gokey)
{
    int i;
    short result;

    if (!id_or_gokey || !*id_or_gokey)
        return -1;

    /* Try numeric parse first */
    if (all_digits(id_or_gokey)) {
        long val = atol(id_or_gokey);
        if (val >= 0 && val < myp->ns)
            return (short)val;
        /* Number out of range -- fall through to name search */
    }

    /* Search by GO key (SubDirName), case-insensitive */
    ObtainSemaphoreShared(&myp->SEM[5]);
    for (i = 0; i < myp->ns; i++) {
        if (myp->Subboard[i].Marker & MRK_SUBBOARD_KILLED)
            continue;
        if (strcasecmp(myp->Subboard[i].SubDirName, id_or_gokey) == 0) {
            ReleaseSemaphore(&myp->SEM[5]);
            return (short)i;
        }
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Fallback: try NumFromUnique() from cnet.library */
    if (CNetBase) {
        result = NumFromUnique((char *)id_or_gokey);
        if (result >= 0 && result < (short)myp->ns)
            return result;
    }

    return -1;
}

void json_error(const char *msg)
{
    struct json_state js;
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "error", msg);
    json_obj_close(&js);
    json_finish(&js);
}

/* ---- warning collection ---- */

static char g_warnings[WARN_MAX][WARN_MSG_SIZE];
static int  g_warn_count = 0;

void warn_clear(void)
{
    g_warn_count = 0;
}

void warn_add(const char *msg)
{
    if (g_warn_count >= WARN_MAX)
        return;
    strncpy(g_warnings[g_warn_count], msg,
        WARN_MSG_SIZE - 1);
    g_warnings[g_warn_count][WARN_MSG_SIZE - 1] = '\0';
    g_warn_count++;
}

void warn_emit(struct json_state *js)
{
    int i;
    if (g_warn_count == 0)
        return;
    json_key(js, "warnings");
    json_arr_open(js);
    for (i = 0; i < g_warn_count; i++)
        json_str(js, g_warnings[i]);
    json_arr_close(js);
}

int warn_count(void)
{
    return g_warn_count;
}

const char *warn_get(int index)
{
    if (index < 0 || index >= g_warn_count)
        return NULL;
    return g_warnings[index];
}

/* ---- Access string conversion ---- */

char *expand_flags_string(char *buf, int bufsz, long flags)
{
    /* ExpandFlags has no size parameter -- it requires at least 128 bytes.
     * Fall back to hex if the buffer is too small. */
    if (CNetBase && bufsz >= 128) {
        ExpandFlags(flags, buf);
    } else {
        snprintf(buf, bufsz, "0x%08lx", (unsigned long)flags);
    }
    return buf;
}

int convert_access_string(const char *s, unsigned long *out)
{
    if (!s || !*s)
        return 0;

    /* Only attempt hex parse if the string has a 0x/0X prefix.
     * This avoids ambiguity: "10" means group 10, not hex 0x10. */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *endp;
        unsigned long val = strtoul(s, &endp, 16);
        if (*endp == '\0') {
            *out = val;
            return 1;
        }
        return 0;
    }

    /* Try ConvertAccess() from cnet.library for group strings
     * like "1-3,5,7-10". */
    if (CNetBase) {
        *out = (unsigned long)ConvertAccess((char *)s);
        return 1;
    }

    return 0;
}

/* ---- UUCP name resolution ---- */

char *handle_to_uucp(char *buf, int bufsz, const char *handle)
{
    char *result;

    if (!CNetBase)
        return NULL;

    result = HNameToUUCP((char *)handle);
    if (!result)
        return NULL;

    /* Copy immediately -- library uses internal storage. */
    strncpy(buf, result, bufsz - 1);
    buf[bufsz - 1] = '\0';
    return buf;
}

char *id_to_uucp(char *buf, int bufsz, long userid)
{
    char *result;

    if (!CNetBase)
        return NULL;

    result = CNetIDToUUCP(userid);
    if (!result)
        return NULL;

    /* Copy immediately -- library uses internal storage. */
    strncpy(buf, result, bufsz - 1);
    buf[bufsz - 1] = '\0';
    return buf;
}

int address_type(const char *addr)
{
    if (!CNetBase)
        return 0;

    return (int)CNetAddressType((char *)addr);
}

/* ---- CNet range functions ---- */

/*
 * Range iteration wraps CNet4 library functions when available,
 * with a simple N or N-M fallback when CNet4Base is NULL.
 *
 * Iteration pattern verified against CNet4 API:
 *   first = parse_range_init(str, min, max, &rctx);
 *   while (1) {
 *       // process first/val
 *       if (parse_range_done(&rctx)) break;
 *       val = parse_range_next(&rctx);
 *   }
 */

long parse_range_init(const char *range_str, long min, long max,
    struct RangeContext *rctx)
{
    if (CNet4Base) {
        long first = CNetFindRange((char *)range_str, min, max, rctx);
        if (first < min || first > max)
            return -1;
        return first;
    }

    /* Fallback: simple N or N-M parser (no comma support). */
    {
        const char *dash;
        long start, end;

        memset(rctx, 0, sizeof(*rctx));

        dash = strchr(range_str, '-');
        if (dash && dash != range_str && dash[1] != '\0') {
            char left_buf[16];
            int left_len = (int)(dash - range_str);
            if (left_len >= (int)sizeof(left_buf))
                left_len = (int)sizeof(left_buf) - 1;
            strncpy(left_buf, range_str, (unsigned)left_len);
            left_buf[left_len] = '\0';

            if (!all_digits(left_buf) || !all_digits(dash + 1))
                return -1;

            start = atol(left_buf);
            end = atol(dash + 1);
        } else {
            if (!all_digits(range_str))
                return -1;
            start = atol(range_str);
            end = start;
        }

        /* Clamp to [min, max] */
        if (start < min) start = min;
        if (end > max) end = max;
        if (start > end)
            return -1;

        rctx->nparts = 1;
        rctx->parts[0][0] = start;
        rctx->parts[0][1] = end;
        rctx->set = 0;
        rctx->element = start;

        return start;
    }
}

long parse_range_next(struct RangeContext *rctx)
{
    if (CNet4Base)
        return CNetNextRange(rctx);

    /* Fallback iteration */
    rctx->element++;
    if (rctx->element > rctx->parts[rctx->set][1]) {
        rctx->set++;
        if (rctx->set >= rctx->nparts)
            return -1;
        rctx->element = rctx->parts[rctx->set][0];
    }
    return rctx->element;
}

int parse_range_done(struct RangeContext *rctx)
{
    if (CNet4Base)
        return CNetEndOfRange(rctx) != 0;

    /* Fallback: done when current element is past the end of the
     * current set AND no more sets remain. */
    if (rctx->set >= rctx->nparts)
        return 1;
    if (rctx->set == rctx->nparts - 1 &&
        rctx->element >= rctx->parts[rctx->set][1])
        return 1;
    return 0;
}
