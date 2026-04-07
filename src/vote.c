/*
 * vote.c -- Voting booth commands for cnet-cli
 *
 * Read-only commands for the global voting booth. Reads vote data files
 * from SysData:Vote/ under SEM[14] shared lock. Three commands:
 *   vote list    -- list all vote topics
 *   vote show    -- topic detail with choices, results, text
 *   vote results -- lightweight results-only view
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "vote.h"
#include "json.h"
#include "util.h"

extern struct Library *CNetBase;

/* ---- on-disk structures ---- */

struct VoteTopicRecord {       /* 94 bytes total */
    char   Name[60];           /*  0: topic name, null-terminated */
    char   _pad[2];            /* 60: alignment padding */
    long   field_62;           /* 62: UNKNOWN (possibly creation date) */
    UBYTE  _unknown_66[6];     /* 66: UNKNOWN (possibly IsDate) */
    long   Serial;             /* 72: unique serial number */
    long   field_76;           /* 76: UNKNOWN */
    long   field_80;           /* 80: UNKNOWN */
    short  field_84;           /* 84: UNKNOWN (possibly current choice count) */
    long   CreatorID;          /* 86: UNKNOWN (probable user IDNumber) */
    UBYTE  Float;              /* 90: global accessibility flag */
    UBYTE  field_91;           /* 91: UNKNOWN (possibly killed flag) */
    short  MaxChoices;         /* 92: maximum choices allowed */
};

_Static_assert(sizeof(struct VoteTopicRecord) == 94,
    "VoteTopicRecord must be 94 bytes");

struct VoteChoiceRecord {      /* 40 bytes */
    char   Text[40];           /*  0: choice text, null-terminated */
};

_Static_assert(sizeof(struct VoteChoiceRecord) == 40,
    "VoteChoiceRecord must be 40 bytes");

struct VoteTalleyRecord {      /* 104 bytes */
    long   UserID;             /*   0: user's IDNumber */
    UBYTE  Votes[100];         /*   4: per-choice vote flags (0 or non-zero) */
};

_Static_assert(sizeof(struct VoteTalleyRecord) == 104,
    "VoteTalleyRecord must be 104 bytes");

/* ---- paths ---- */

#define VOTE_TOPICS_PATH "SysData:Vote/topics"

/*
 * Build a path to a file within a topic directory.
 * Result: "SysData:Vote/{topic_num}/{filename}"
 */
static void build_topic_path(char *buf, int bufsz, int topic_num,
    const char *filename)
{
    snprintf(buf, bufsz, "SysData:Vote/%d/%s", topic_num, filename);
}

/* ---- hex helpers ---- */

/*
 * Format a long (4 bytes) as 8-char lowercase hex.
 */
static void format_hex_long(char *buf, int bufsz, long val)
{
    snprintf(buf, bufsz, "%08lx", (unsigned long)val);
}

/*
 * Format a 6-byte region as 12-char lowercase hex.
 */
static void format_hex_6bytes(char *buf, int bufsz, const UBYTE *data)
{
    snprintf(buf, bufsz, "%02x%02x%02x%02x%02x%02x",
        data[0], data[1], data[2], data[3], data[4], data[5]);
}

/* ---- topic field emitters ---- */

/*
 * Emit the common topic fields to the JSON state.
 * Used by both cmd_vote_list and cmd_vote_show.
 */
static void emit_topic_fields(struct json_state *js,
    const struct VoteTopicRecord *topic, int number)
{
    char buf[128];
    char hexbuf[16];

    json_kv_int(js, "number", (long)number);
    json_kv_str(js, "name",
        strip_mci(buf, sizeof(buf), topic->Name));
    json_kv_int(js, "serial", topic->Serial);
    json_kv_bool(js, "float", (int)topic->Float);
    json_kv_int(js, "max_choices", (long)topic->MaxChoices);

    /* Unknown fields as hex */
    format_hex_long(hexbuf, sizeof(hexbuf), topic->field_62);
    json_kv_str(js, "field_62_hex", hexbuf);

    format_hex_6bytes(hexbuf, sizeof(hexbuf), topic->_unknown_66);
    json_kv_str(js, "field_66_hex", hexbuf);

    format_hex_long(hexbuf, sizeof(hexbuf), topic->field_76);
    json_kv_str(js, "field_76_hex", hexbuf);

    format_hex_long(hexbuf, sizeof(hexbuf), topic->field_80);
    json_kv_str(js, "field_80_hex", hexbuf);

    json_kv_int(js, "field_84", (long)topic->field_84);

    format_hex_long(hexbuf, sizeof(hexbuf), topic->CreatorID);
    json_kv_str(js, "field_86_hex", hexbuf);

    json_kv_int(js, "field_91", (long)topic->field_91);
}

/* ---- commands ---- */

int cmd_vote_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct VoteTopicRecord topic;
    BPTR fh;
    int number = 1;
    int emitted_count = 0;

    (void)argc;
    (void)argv;

    ObtainSemaphoreShared(&myp->SEM[14]);

    fh = Open((CONST_STRPTR)VOTE_TOPICS_PATH, MODE_OLDFILE);
    if (!fh) {
        /* No topics file -- emit empty result */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_key(&js, "topics");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_obj_close(&js);
        json_finish(&js);
        ReleaseSemaphore(&myp->SEM[14]);
        return 0;
    }

    /* Check file size alignment */
    {
        long fsize_list;
        Seek(fh, 0, OFFSET_END);
        fsize_list = Seek(fh, 0, OFFSET_BEGINNING);
        if (fsize_list % (long)sizeof(struct VoteTopicRecord) != 0)
            warn_add("Topics file size is not a multiple of record size");
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "topics");
    json_arr_open(&js);

    while (Read(fh, &topic, sizeof(struct VoteTopicRecord))
            == (long)sizeof(struct VoteTopicRecord)) {
        json_obj_open(&js);
        emit_topic_fields(&js, &topic, number);
        json_obj_close(&js);

        emitted_count++;
        number++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "count", (long)emitted_count);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    Close(fh);
    ReleaseSemaphore(&myp->SEM[14]);
    return 0;
}

int cmd_vote_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct VoteTopicRecord topic;
    struct VoteChoiceRecord choice;
    struct VoteTalleyRecord talley;
    BPTR fh;
    long topic_num;
    long offset;
    long fsize;
    long max_topics;
    long total;
    int choice_count;
    int voter_count;
    long total_votes;
    int i;
    char pathbuf[128];
    char textbuf[4096];
    long text_len;

    if (argc < 2 || !all_digits(argv[1])) {
        json_error("Usage: cnet-cli vote show <topic-number>");
        return 1;
    }

    topic_num = atol(argv[1]);
    if (topic_num < 1) {
        json_error("Topic number must be >= 1");
        return 1;
    }

    ObtainSemaphoreShared(&myp->SEM[14]);

    /* Read the topic record */
    fh = Open((CONST_STRPTR)VOTE_TOPICS_PATH, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&myp->SEM[14]);
        json_error("No vote topics configured");
        return 1;
    }

    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    max_topics = fsize / (long)sizeof(struct VoteTopicRecord);

    if (fsize % (long)sizeof(struct VoteTopicRecord) != 0)
        warn_add("Topics file size is not a multiple of record size");

    if (topic_num > max_topics) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[14]);
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Vote topic %ld out of range (1-%ld)",
                topic_num, max_topics);
            json_error(buf);
        }
        return 1;
    }

    /* Seek to the requested record (1-based: topic 1 is at offset 0) */
    offset = (topic_num - 1) * (long)sizeof(struct VoteTopicRecord);
    Seek(fh, offset, OFFSET_BEGINNING);

    if (Read(fh, &topic, sizeof(struct VoteTopicRecord))
            != (long)sizeof(struct VoteTopicRecord)) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[14]);
        json_error("Failed to read vote topic");
        return 1;
    }

    Close(fh);

    /* Begin JSON output */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "topic");
    json_obj_open(&js);

    emit_topic_fields(&js, &topic, (int)topic_num);

    /* Read choices */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "choices");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    json_key(&js, "choices");
    json_arr_open(&js);

    choice_count = 0;
    if (fh) {
        while (Read(fh, &choice, sizeof(struct VoteChoiceRecord))
                == (long)sizeof(struct VoteChoiceRecord)) {
            choice_count++;
            json_obj_open(&js);
            json_kv_int(&js, "number", (long)choice_count);
            {
                char cbuf[64];
                json_kv_str(&js, "text",
                    strip_mci(cbuf, sizeof(cbuf), choice.Text));
            }
            json_obj_close(&js);
        }
        Close(fh);
    }

    json_arr_close(&js);
    json_kv_int(&js, "choice_count", (long)choice_count);

    /* Read totals */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "totals");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    json_key(&js, "results");
    json_arr_open(&js);

    total_votes = 0;
    if (fh) {
        for (i = 0; i < choice_count; i++) {
            total = 0;
            if (Read(fh, &total, 4) == 4) {
                json_int(&js, total);
                total_votes += total;
            }
        }
        Close(fh);
    }

    json_arr_close(&js);
    json_kv_int(&js, "total_votes", total_votes);

    /* Count voters from talley file */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "talley");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    voter_count = 0;
    if (fh) {
        while (Read(fh, &talley, sizeof(struct VoteTalleyRecord))
                == (long)sizeof(struct VoteTalleyRecord)) {
            voter_count++;
        }
        Close(fh);
    }

    json_kv_int(&js, "voter_count", (long)voter_count);

    /* Read text file */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "text");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    if (fh) {
        text_len = Read(fh, textbuf, sizeof(textbuf) - 1);
        Close(fh);

        if (text_len > 0) {
            textbuf[text_len] = '\0';
            json_kv_str(&js, "text", textbuf);
        } else {
            json_kv_null(&js, "text");
        }
    } else {
        json_kv_null(&js, "text");
    }

    json_obj_close(&js);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[14]);
    return 0;
}

int cmd_vote_results(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct VoteTopicRecord topic;
    struct VoteChoiceRecord choice;
    struct VoteTalleyRecord talley;
    BPTR fh;
    long topic_num;
    long offset;
    long fsize;
    long max_topics;
    long total;
    int choice_count;
    int voter_count;
    long total_votes;
    int i;
    char pathbuf[128];

    if (argc < 2 || !all_digits(argv[1])) {
        json_error("Usage: cnet-cli vote results <topic-number>");
        return 1;
    }

    topic_num = atol(argv[1]);
    if (topic_num < 1) {
        json_error("Topic number must be >= 1");
        return 1;
    }

    ObtainSemaphoreShared(&myp->SEM[14]);

    /* Read the topic record to validate the topic number */
    fh = Open((CONST_STRPTR)VOTE_TOPICS_PATH, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&myp->SEM[14]);
        json_error("No vote topics configured");
        return 1;
    }

    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    max_topics = fsize / (long)sizeof(struct VoteTopicRecord);

    if (fsize % (long)sizeof(struct VoteTopicRecord) != 0)
        warn_add("Topics file size is not a multiple of record size");

    if (topic_num > max_topics) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[14]);
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Vote topic %ld out of range (1-%ld)",
                topic_num, max_topics);
            json_error(buf);
        }
        return 1;
    }

    /* Read the topic (needed for validation only) */
    offset = (topic_num - 1) * (long)sizeof(struct VoteTopicRecord);
    Seek(fh, offset, OFFSET_BEGINNING);

    if (Read(fh, &topic, sizeof(struct VoteTopicRecord))
            != (long)sizeof(struct VoteTopicRecord)) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[14]);
        json_error("Failed to read vote topic");
        return 1;
    }

    Close(fh);

    (void)topic; /* topic read for validation / future use */

    /* Read choices to get choice texts and count */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "choices");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    /* Store choice texts for the results output */
    {
        char choice_texts[100][40];
        choice_count = 0;

        if (fh) {
            while (choice_count < 100 &&
                    Read(fh, &choice, sizeof(struct VoteChoiceRecord))
                    == (long)sizeof(struct VoteChoiceRecord)) {
                memcpy(choice_texts[choice_count], choice.Text, 40);
                choice_count++;
            }
            Close(fh);
        }

        /* Begin JSON output */
        json_init(&js, stdout);
        json_obj_open(&js);

        json_kv_int(&js, "topic_number", topic_num);

        json_key(&js, "results");
        json_arr_open(&js);

        /* Read totals and emit per-choice results */
        build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num,
            "totals");
        fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

        total_votes = 0;
        for (i = 0; i < choice_count; i++) {
            total = 0;
            if (fh)
                Read(fh, &total, 4);

            json_obj_open(&js);
            json_kv_int(&js, "choice", (long)(i + 1));
            {
                char cbuf[64];
                json_kv_str(&js, "text",
                    strip_mci(cbuf, sizeof(cbuf), choice_texts[i]));
            }
            json_kv_int(&js, "votes", total);
            json_obj_close(&js);

            total_votes += total;
        }

        if (fh)
            Close(fh);

        json_arr_close(&js);
        json_kv_int(&js, "total_votes", total_votes);
    }

    /* Count voters from talley file */
    build_topic_path(pathbuf, sizeof(pathbuf), (int)topic_num, "talley");
    fh = Open((CONST_STRPTR)pathbuf, MODE_OLDFILE);

    voter_count = 0;
    if (fh) {
        while (Read(fh, &talley, sizeof(struct VoteTalleyRecord))
                == (long)sizeof(struct VoteTalleyRecord)) {
            voter_count++;
        }
        Close(fh);
    }

    json_kv_int(&js, "voter_count", (long)voter_count);

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[14]);
    return 0;
}
