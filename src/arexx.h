/*
 * arexx.h -- ARexx IPC commands for cnet-cli
 *
 * ARexx IPC operations: send, control
 */

#ifndef CNET_CLI_AREXX_H
#define CNET_CLI_AREXX_H

#include <exec/types.h>

struct MainPort;

/*
 * Send an ARexx command to a named port and wait for the reply.
 *
 * portname:   target ARexx port (e.g., "CNETREXX0", "CONTROLREXX.1")
 * command:    command string to send
 * out_rc:     receives rm_Result1 (return code)
 * result_buf: receives rm_Result2 string (may be empty if no result)
 * buf_size:   size of result_buf
 *
 * Returns 0 on success (message sent and reply received).
 * Returns -1 on infrastructure failure (library not open, allocation
 *   failure, target port not found).
 *
 * On success, *out_rc contains the ARexx return code.
 * On infrastructure failure, *out_rc is undefined.
 */
int send_arexx_command(const char *portname, const char *command,
                       LONG *out_rc, char *result_buf, int buf_size);

int cmd_arexx_send(struct MainPort *myp, int argc, char **argv);
int cmd_arexx_control(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_AREXX_H */
