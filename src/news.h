/*
 * news.h -- News/GFile/PFile (text/door) commands for cnet-cli
 *
 * Phase 9: news list, read, post, edit, delete
 */

#ifndef CNET_CLI_NEWS_H
#define CNET_CLI_NEWS_H

struct MainPort;

/*
 * Command handlers for news/text-door operations.
 * Each takes argc/argv from the sub-command (after "news").
 * e.g., "cnet-cli news list TESTNEWS" -> argc=2, argv={"list","TESTNEWS"}
 *
 * myp must be valid (MainPort found).
 * Returns 0 on success, nonzero on error.
 */

/* Read operations */
int cmd_news_list(struct MainPort *myp, int argc, char **argv);
int cmd_news_read(struct MainPort *myp, int argc, char **argv);

/* Mutation operations */
int cmd_news_post(struct MainPort *myp, int argc, char **argv);
int cmd_news_edit(struct MainPort *myp, int argc, char **argv);
int cmd_news_delete(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_NEWS_H */
