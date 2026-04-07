/*
 * util.h -- String utilities for cnet-cli
 *
 * MCI code stripping, safe string copy, date formatting.
 */

#ifndef CNET_CLI_UTIL_H
#define CNET_CLI_UTIL_H

/* Forward declaration -- full definition via <cnet/cnet.h> in .c files */
struct MainPort;

/*
 * Strip MCI codes from a CNet string using MCIRemove() from cnet.library.
 * Copies src into buf (up to bufsz-1 chars), then strips in-place.
 * Returns buf for convenience.
 *
 * If CNetBase is NULL (cnet.library not open), just copies without stripping.
 */
char *strip_mci(char *buf, int bufsz, const char *src);

/*
 * Format an IsDate as "YYYY-MM-DDTHH:MM:SS" into buf.
 * buf must be at least 20 bytes.
 * Returns buf.
 *
 * If date is all zeros, returns "null" representation -- caller should
 * use json_kv_null() instead if checking is_null_date().
 */
char *format_date(char *buf, int bufsz, const void *date_ptr);

/*
 * Check if an IsDate is all zeros (unset).
 */
int is_null_date(const void *date_ptr);

/*
 * Emit a JSON error object {"error": "<msg>"} to stdout.
 * Shared helper used by command handlers across multiple source files.
 */
void json_error(const char *msg);

/*
 * Warning collection.  Non-fatal warnings are accumulated during
 * command execution and emitted in the success JSON response.
 */
#define WARN_MAX      8
#define WARN_MSG_SIZE 128

struct json_state;

void warn_clear(void);
void warn_add(const char *msg);
void warn_emit(struct json_state *js);
int  warn_count(void);
const char *warn_get(int index);

/*
 * Return a human-readable name for a subboard marker base type.
 */
const char *marker_type_name(int marker_base);

/*
 * Check if a string is all decimal digits.
 * Returns 1 if numeric, 0 otherwise. Empty string returns 0.
 */
int all_digits(const char *s);

/*
 * Case-insensitive substring search.
 * Returns 1 if needle is found within haystack, 0 otherwise.
 * Empty needle matches everything.
 */
int ci_contains(const char *haystack, const char *needle);

/*
 * Safe string copy for fixed-size char arrays.
 * Always null-terminates within maxlen bytes.
 */
void safe_strcpy(char *dest, const char *src, int maxlen);

/*
 * Resolve a user identifier (account number or handle) to an account number.
 * Optionally retrieves the user's UUCP name.
 *
 * If out_uucp is non-NULL, copies the UUCP name from Key[account-1].UUCP
 * into out_uucp (up to uucp_bufsz-1 chars, null-terminated).
 * Pass out_uucp=NULL and uucp_bufsz=0 to skip UUCP lookup.
 *
 * Returns account number (>= 1) on success, or -1 if not found.
 */
short resolve_user_full(struct MainPort *myp, const char *id_or_handle,
    char *out_uucp, int uucp_bufsz);

/*
 * Write a single SubboardType4 record to disk at the given physnum offset
 * in SysData:subboards4. Used by both subboard mutation commands and message
 * commands that update subboard counts.
 *
 * Must be called under SEM[5] exclusive lock when concurrent access is possible.
 *
 * Returns 0 on success, -1 on I/O error.
 */
struct SubboardType4;
int write_subboard_disk(int physnum, struct SubboardType4 *sub);

/*
 * Resolve a subboard identifier to a physical subboard number.
 *
 * Accepts either a numeric string (physical number) or a GO key
 * string (SubDirName, case-insensitive match). Falls back to
 * NumFromUnique() from cnet.library.
 *
 * Acquires SEM[5] shared internally for the GO key search.
 *
 * Returns the physical subboard number (>= 0) or -1 if not found.
 */
short resolve_subboard(struct MainPort *myp, const char *id_or_gokey);

/*
 * Build a path to a file under a subboard's data/ directory.
 * Handles AmigaOS path joining: volume: needs no separator,
 * directory/ needs no extra separator.
 *
 * Result: "{data_path}data/{filename}" or "{data_path}/data/{filename}"
 */
void build_data_file_path(char *buf, int bufsz,
    const char *data_path, const char *filename);

/* ---- Access string conversion ---- */

/*
 * Convert an access bitmask to a human-readable string using ExpandFlags()
 * from cnet.library. Output format: "1-3,5,7-10" (group numbers 0-31).
 *
 * Falls back to hex format "0xNNNNNNNN" if CNetBase is NULL.
 * buf must be at least 128 bytes.
 * Returns buf.
 */
char *expand_flags_string(char *buf, int bufsz, long flags);

/*
 * Parse an access string (either hex "0xNNNNNNNN" or human-readable
 * "1-3,5,7-10") into a bitmask using ConvertAccess() from cnet.library.
 *
 * Tries hex parse first (only for 0x/0X prefix). Otherwise uses
 * ConvertAccess() if CNetBase is available.
 *
 * Returns 1 on success (value in *out), 0 on failure.
 */
int convert_access_string(const char *s, unsigned long *out);

/* ---- UUCP name resolution ---- */

/*
 * Look up a UUCP name from a user handle using HNameToUUCP() from
 * cnet.library. Copies the result into buf (up to bufsz-1 chars).
 * Returns buf on success, or NULL if not found or CNetBase is NULL.
 */
char *handle_to_uucp(char *buf, int bufsz, const char *handle);

/*
 * Look up a UUCP name from a user ID number using CNetIDToUUCP() from
 * cnet.library. Same semantics as handle_to_uucp().
 */
char *id_to_uucp(char *buf, int bufsz, long userid);

/*
 * Classify an address string using CNetAddressType() from cnet.library.
 * Returns the address type code, or 0 if CNetBase is NULL.
 */
int address_type(const char *addr);

/* ---- CNet range functions ---- */

/*
 * Parse a range string using CNet's native range parser (cnet4.library).
 * Supports CNet-style ranges: "1-5", "1,3,5-10".
 * Caller must handle "all" before calling this.
 *
 * Falls back to simple N or N-M custom parser if CNet4Base is NULL.
 *
 * Initializes the RangeContext for iteration. Returns the first value,
 * or -1 on parse error.
 *
 * min/max clamp the range to valid bounds.
 */
struct RangeContext;
long parse_range_init(const char *range_str, long min, long max,
    struct RangeContext *rctx);

/*
 * Get the next value from an initialized RangeContext.
 * Returns the next value, or -1 if the range is exhausted.
 */
long parse_range_next(struct RangeContext *rctx);

/*
 * Check if a RangeContext has been exhausted.
 * Returns 1 if no more values, 0 if more values remain.
 */
int parse_range_done(struct RangeContext *rctx);

#endif /* CNET_CLI_UTIL_H */
