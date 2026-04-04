/*
 * gfile.h -- GFile (General Text File) commands for cnet-cli
 *
 * Phase 14: gfile list, read, add, remove
 */

#ifndef CNET_CLI_GFILE_H
#define CNET_CLI_GFILE_H

struct MainPort;

int cmd_gfile_list(struct MainPort *myp, int argc, char **argv);
int cmd_gfile_read(struct MainPort *myp, int argc, char **argv);
int cmd_gfile_add(struct MainPort *myp, int argc, char **argv);
int cmd_gfile_remove(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_GFILE_H */
