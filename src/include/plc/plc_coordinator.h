/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2019-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */

#ifndef _CO_COORDINATOR_H
#define _CO_COORDINATOR_H

/* the container status key */
typedef struct ContainerKey
{
	pid_t       qe_pid;
	int         conn;
	int 		ccnt;
} ContainerKey;

/* the container status entry */
typedef struct ContainerEntry
{
	ContainerKey    key;		        /* hash key */
	char            containerId[16];    /* for container */
	pid_t           stand_alone_pid;    /* for stand alone mode */
	char* 			status;
} ContainerEntry;

extern char *get_coordinator_address(void);
extern int start_container(const char *runtimeid, pid_t qe_pid, int session_id, int ccnt, int dbid, const char *ownername, char **uds_address, char **container_id, char **log_msg);
extern int destroy_container(pid_t qe_pid, int session_id, int ccnt);

#endif /* _CO_COORDINATOR_H */
