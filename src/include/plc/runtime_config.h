#ifndef __RUNTIME_CONFIG_H__
#define __RUNTIME_CONFIG_H__
#define RUNTIME_ID_MAX_LENGTH 64
typedef enum {
    PLC_ACCESS_READONLY = 0,
    PLC_ACCESS_READWRITE = 1
} plcFsAccessMode;

typedef enum {
    PLC_INSPECT_STATUS = 0,
    PLC_INSPECT_PORT = 1,
    PLC_INSPECT_NAME = 2,
    PLC_INSPECT_OOM = 3,
    PLC_INSPECT_PORT_UNKNOWN,
} plcInspectionMode;

typedef struct plcSharedDir {
    char *host;
    char *container;
    plcFsAccessMode mode;
} plcSharedDir;

/*
* Struct runtimeConfEntry is the entry of hash table.
* The key of hash table must be the first field of struct.
*/
typedef struct runtimeConfEntry {
    char runtimeid[RUNTIME_ID_MAX_LENGTH];
    char *image;
    char *command;
    char *roles;
    int resgroupOid;
    int memoryMb;
    int cpuShare;
    int nSharedDirs;
    plcSharedDir *sharedDirs;
    bool useContainerNetwork;
    bool useContainerLogging;
    bool useUserControl;
} runtimeConfEntry;
#endif