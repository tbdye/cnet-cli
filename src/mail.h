/*
 * mail.h -- Mail commands for cnet-cli
 *
 * Mail operations: send, list, read, reply, delete, folders, count, feedback, verify
 */

#ifndef CNET_CLI_MAIL_H
#define CNET_CLI_MAIL_H

struct MainPort;

/* Mail command handlers */
int cmd_mail_send(struct MainPort *myp, int argc, char **argv);
int cmd_mail_list(struct MainPort *myp, int argc, char **argv);
int cmd_mail_read(struct MainPort *myp, int argc, char **argv);
int cmd_mail_reply(struct MainPort *myp, int argc, char **argv);
int cmd_mail_delete(struct MainPort *myp, int argc, char **argv);
int cmd_mail_folders(struct MainPort *myp, int argc, char **argv);
int cmd_mail_count(struct MainPort *myp, int argc, char **argv);

/* Mail wrapper commands */
int cmd_mail_feedback(struct MainPort *myp, int argc, char **argv);
int cmd_mail_verify(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_MAIL_H */
