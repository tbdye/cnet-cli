/*
 * maint.h -- Maintenance commands for cnet-cli
 *
 * Maintenance operations: rebuild indexes, recount, repair data files.
 */

#ifndef CNET_CLI_MAINT_H
#define CNET_CLI_MAINT_H

struct MainPort;

/* Maintenance subcommands */
int cmd_maint_pointers(struct MainPort *myp, int argc, char **argv);
int cmd_maint_count(struct MainPort *myp, int argc, char **argv);
int cmd_maint_repair_mail(struct MainPort *myp, int argc, char **argv);
int cmd_maint_repair_sub(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_MAINT_H */
