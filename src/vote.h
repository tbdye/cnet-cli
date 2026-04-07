/*
 * vote.h -- Voting booth commands for cnet-cli
 *
 * Read-only vote topic listing, detail, and results.
 */

#ifndef CNET_CLI_VOTE_H
#define CNET_CLI_VOTE_H

struct MainPort;

int cmd_vote_list(struct MainPort *myp, int argc, char **argv);
int cmd_vote_show(struct MainPort *myp, int argc, char **argv);
int cmd_vote_results(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_VOTE_H */
