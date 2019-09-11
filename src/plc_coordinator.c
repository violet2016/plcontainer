/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2019-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */

#include "postgres.h"
#include <unistd.h>

#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/idle_resource_cleaner.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "common/base_network.h"
#include "plc/plc_configuration.h"
#include "plc/plc_coordinator.h"
#include "common/comm_shm.h"
#include "common/messages/messages.h"

PG_MODULE_MAGIC;
// PROTOTYPE:
extern void _PG_init(void);
extern void plc_coordinator_main(Datum datum);
extern void plc_coordinator_aux_main(Datum datum);

// END OF PROTOTYPES.

static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

CoordinatorStruct *coordinator_shm;

#define RECEIVE_BUF_SIZE 2048
#define TIMEOUT_SEC 3
char* receiveBuffer = NULL;

static void
plc_coordinator_shmem_startup(void)
{
    bool found;
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    // TODO: do shared memory initialization
    coordinator_shm = ShmemInitStruct(CO_SHM_KEY, MAXALIGN(sizeof(CoordinatorStruct)), &found);
    Assert(!found);
    coordinator_shm->state = CO_STATE_UNINITIALIZED;
}

static void
request_shmem_(void)
{
    RequestAddinShmemSpace(MAXALIGN(sizeof(CoordinatorStruct)));

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = plc_coordinator_shmem_startup;
}
static void
plc_coordinator_sigterm(pg_attribute_unused() SIGNAL_ARGS)
{
    got_sigterm = true;
}

static void
plc_coordinator_sighup(pg_attribute_unused() SIGNAL_ARGS)
{
    got_sighup = true;
}

static int
plc_listen_socket()
{
    char address[500];
    int sock;
    snprintf(address, sizeof(address), "/tmp/.plcoordinator.%ld.unix.sock", (long)getpid());
    sock = plcListenServer("unix", address);
    if (sock < 0) {
        plc_elog(ERROR, "initialize socket failed");
    }
    coordinator_shm->protocol = CO_PROTO_UNIX;
    strcpy(coordinator_shm->address, address);
    return sock;
}

/**
 * Initialize coordinator:
 * 1. Create socket to listen request from QE
 * 2. Create coordinator aux process to do some hard work,
 *     like inspect docker
 */
static int
plc_initialize_coordinator()
{
    BackgroundWorker auxWorker;
    int sock;

    sock = plc_listen_socket();

    if (plc_refresh_container_config(false) != 0) {
        if (runtime_conf_table == NULL) {
            /* can't load runtime configuration */
            plc_elog(WARNING, "PL/container: can't load runtime configuration");
        } else {
            plc_elog(WARNING, "PL/container: there is no runtime configuration");
        }
    }

    memset(&auxWorker, 0, sizeof(auxWorker));
    auxWorker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    auxWorker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    auxWorker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
    auxWorker.bgw_main = plc_coordinator_aux_main;

    auxWorker.bgw_notify_pid = 0;
    snprintf(auxWorker.bgw_name, sizeof(auxWorker.bgw_name), "plcoordinator_aux");
    RegisterDynamicBackgroundWorker(&auxWorker, NULL);
    receiveBuffer = palloc(RECEIVE_BUF_SIZE);
    if (!receiveBuffer)
    {
        plc_elog(ERROR, "failed allocating memory in coordinator\n");
    }
    return sock;
}

static void process_msg_from_sock(int sock, void *ptr, size_t len)
{
    char msg_type;
    (void) ptr;
    ssize_t sz = 0;
    struct sockaddr_un remote;
    size_t len_addr = sizeof(struct sockaddr_un);
    int s2 = accept(sock, &remote, &len_addr);
    if (s2 < 0) {
        return;
    }
    if ((sz=recv(s2, &msg_type, 1, 0)) < 0)
    {
        plc_elog(INFO, "Failed to recv data: %s", strerror(errno));
        return;
    }

    switch (msg_type) {
        case MT_PLCID:
            plcMsgPLCId* mplc_id = palloc(sizeof(plcMsgPLCId));
            recv(s2, &mplc_id->sessionid, sizeof(plcMsgPLCId)- sizeof(plcMessage), 0);
            plc_elog(DEBUG1, "receive id msg from process %d", mplc_id->pid);
            // TODO: add create container function
            break;
        default:
            break;
    }
    if (shutdown(s2, 2) < 0)
    {
        plc_elog(ERROR, "error in shutdown uds");
    }
    close(s2);
}
static int wait_for_msg(int sock) {
    struct timeval timeout;
    int rv;
    fd_set fdset;

    FD_ZERO(&fdset);    /* clear the set */
    FD_SET(sock, &fdset); /* add our file descriptor to the set */
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;

    rv = select(sock + 1, &fdset, NULL, NULL, &timeout);
    if (rv == -1) {
        plc_elog(ERROR, "Failed to select() socket: %s", strerror(errno));
    }
    return rv;
}
void
plc_coordinator_main(Datum datum)
{
    int sock, rc;
    (void)datum;
    pqsignal(SIGTERM, plc_coordinator_sigterm);
    pqsignal(SIGHUP, plc_coordinator_sighup);
    sock = plc_initialize_coordinator();
    BackgroundWorkerUnblockSignals();

    coordinator_shm->state = CO_STATE_READY;
    plc_elog(INFO, "plcoordinator is going to enter main loop, sock=%d", sock);
    //struct sockaddr_in cli_addr;
    //socklen_t addrlen = sizeof(cli_addr);
    while(!got_sigterm) {
        if (wait_for_msg(sock) > 0)
        {
            process_msg_from_sock(sock, receiveBuffer, RECEIVE_BUF_SIZE);
        }
        rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0);
        if (rc & WL_POSTMASTER_DEATH)
            break;
        ResetLatch(&MyProc->procLatch);
        if (got_sighup) {
            got_sighup = false;
        }
        /* TODO: Replace with coordinator logic here */
        sleep(2);
    }

    if (coordinator_shm->protocol != CO_PROTO_TCP)
        unlink(coordinator_shm->address);
    proc_exit(0);
}

void
plc_coordinator_aux_main(Datum datum)
{
    int rc;
    (void)datum;
    pqsignal(SIGTERM, plc_coordinator_sigterm);
    pqsignal(SIGHUP, SIG_IGN);
    BackgroundWorkerUnblockSignals();
    // TODO: impl coordinator logic here
    while(!got_sigterm) {
        rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0);
        if (rc & WL_POSTMASTER_DEATH)
            break;
        ResetLatch(&MyProc->procLatch);
        sleep(2);
    }

    proc_exit(0);
}

void
_PG_init(void)
{
    BackgroundWorker worker;

    request_shmem_();
    memset(&worker, 0, sizeof(BackgroundWorker));

    /* coordinator.so must be in shared_preload_libraries to init SHM. */
    if (!process_shared_preload_libraries_in_progress)
        ereport(ERROR, (errmsg("plc_coordinator.so not in shared_preload_libraries.")));

    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
    worker.bgw_main = plc_coordinator_main;

    worker.bgw_notify_pid = 0;

    snprintf(worker.bgw_name, BGW_MAXLEN, "[plcontainer] - coordinator");

    RegisterBackgroundWorker(&worker);
    plc_elog(NOTICE, "init plc_coordinator %d done", (int)getpid());
}
