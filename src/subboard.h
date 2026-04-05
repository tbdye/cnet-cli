/*
 * subboard.h -- Subboard commands for cnet-cli
 *
 * Subboard operations: list, show, tree, path, create, edit, delete
 */

#ifndef CNET_CLI_SUBBOARD_H
#define CNET_CLI_SUBBOARD_H

struct MainPort;

/*
 * Command handlers for subboard operations.
 * Each takes argc/argv from the sub-command (after "sub").
 * e.g., "cnet-cli sub list --active" -> argc=2, argv={"list","--active"}
 *
 * myp must be valid (MainPort found).
 * Returns 0 on success, nonzero on error.
 */

/* Read operations */
int cmd_sub_list(struct MainPort *myp, int argc, char **argv);
int cmd_sub_show(struct MainPort *myp, int argc, char **argv);
int cmd_sub_tree(struct MainPort *myp, int argc, char **argv);

/* Read operations (continued) */
int cmd_sub_path(struct MainPort *myp, int argc, char **argv);

/* Mutation operations */
int cmd_sub_create(struct MainPort *myp, int argc, char **argv);
int cmd_sub_edit(struct MainPort *myp, int argc, char **argv);
int cmd_sub_delete(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_SUBBOARD_H */
