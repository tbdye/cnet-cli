/*
 * message.h -- Message commands for cnet-cli
 *
 * Message operations: list, read, post, respond, delete, edit, search, move
 */

#ifndef CNET_CLI_MESSAGE_H
#define CNET_CLI_MESSAGE_H

struct MainPort;

/*
 * Command handlers for message operations.
 * Each takes argc/argv from the sub-command (after "msg").
 * e.g., "cnet-cli msg list General" -> argc=2, argv={"list","General"}
 *
 * myp must be valid (MainPort found).
 * Returns 0 on success, nonzero on error.
 */

/* Read operations */
int cmd_msg_list(struct MainPort *myp, int argc, char **argv);
int cmd_msg_read(struct MainPort *myp, int argc, char **argv);

/* Mutation operations */
int cmd_msg_post(struct MainPort *myp, int argc, char **argv);
int cmd_msg_respond(struct MainPort *myp, int argc, char **argv);
int cmd_msg_delete(struct MainPort *myp, int argc, char **argv);
int cmd_msg_edit(struct MainPort *myp, int argc, char **argv);
int cmd_msg_move(struct MainPort *myp, int argc, char **argv);

/* Search operations */
int cmd_msg_search(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_MESSAGE_H */
