/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */
#ifndef PLC_COMM_CONNECTIVITY_H
#define PLC_COMM_CONNECTIVITY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define DEFAULT_STRING_BUFFER_SIZE 1024
#define MAX_LOG_LENGTH 1024
#define MAX_PLC_CONTEXT_STAGE_NUM           16
#define MAX_PLC_CONTEXT_STAGE_NAME_SIZE     128
#define MAX_PLC_CONTEXT_STAGE_MESSAGE_SIZE  1024

typedef enum plcContextStageStatus {
    PLC_CONTEXT_STAGE_SUCCESS,
    PLC_CONTEXT_STAGE_FAIL,
    PLC_CONTEXT_STAGE_TIMEOUT
} plcContextStageStatus;

typedef struct plcContextStage {
    char name[MAX_PLC_CONTEXT_STAGE_NAME_SIZE];
    struct timeval begin_time;
    struct timeval end_time;
    int cost_ms;
    plcContextStageStatus   status;
    char message[MAX_PLC_CONTEXT_STAGE_MESSAGE_SIZE];
} plcContextStage;

typedef struct plcContext
{
	char *service_address; /* File for unix domain socket connection only. */
    char *container_id;
    plcContextStage stages[MAX_PLC_CONTEXT_STAGE_NUM];
    int current_stage_num;
    int max_stage_num;
} plcContext;

extern plcContext *global_context;

#define UDS_SHARED_FILE "unix.domain.socket.shared.file"
#define IPC_CLIENT_DIR "/tmp/plcontainer"
#define IPC_GPDB_BASE_DIR "/tmp/plcontainer"
#define MAX_SHARED_FILE_SZ strlen(UDS_SHARED_FILE)

extern void plcContextInit(plcContext *ctx);
extern void plcFreeContext(plcContext *ctx);
extern void plcReleaseContext(plcContext *ctx);
extern void plcContextReset(plcContext *ctx);

extern void plcContextBeginStage(plcContext *ctx, const char *stage_name, const char *message_format, ...);
extern void plcContextEndStage(plcContext *ctx, const char *stage_name, plcContextStageStatus status, const char *message_queue_status, ...);
extern void plcContextLogging(int log_level, plcContext *ctx);

#endif /* PLC_COMM_CONNECTIVITY_H */
