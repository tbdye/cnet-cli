/*
 * file.h -- File area commands for cnet-cli
 *
 * File area operations: list, show, add, edit, remove, validate, find
 */

#ifndef CNET_CLI_FILE_H
#define CNET_CLI_FILE_H

struct MainPort;

/*
 * Command handlers for file area operations.
 * Each takes argc/argv from the sub-command (after "file").
 * e.g., "cnet-cli file list UL" -> argc=2, argv={"list","UL"}
 *
 * myp must be valid (MainPort found).
 * Returns 0 on success, nonzero on error.
 */

/* Read operations */
int cmd_file_list(struct MainPort *myp, int argc, char **argv);
int cmd_file_show(struct MainPort *myp, int argc, char **argv);

/* Mutation operations */
int cmd_file_add(struct MainPort *myp, int argc, char **argv);
int cmd_file_edit(struct MainPort *myp, int argc, char **argv);
int cmd_file_remove(struct MainPort *myp, int argc, char **argv);
int cmd_file_validate(struct MainPort *myp, int argc, char **argv);

/* Search operations */
int cmd_file_find(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_FILE_H */
