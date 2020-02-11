#ifndef __PLC_DOCKER_H__
#define __PLC_DOCKER_H__


#if defined(__cplusplus)
extern "C" {
#endif
    
	int PlcDocker_create(runtimeConfEntry *conf, char **name, char *uds_dir, int qe_pid, int session_id, int ccnt, int uid, int gid,int procid, int dbid, char *ownername);
    int PlcDocker_start(const char *id, char *msg);
    int PlcDocker_delete(const char **ids, int length, char *msg);
    int PlcDocker_stat(const char** ids, int length, int64_t *mem_usage);
#if defined(__cplusplus)
}
#endif
#include <string>
#include "docker/docker_client.h"
class PlcDocker {
public:
    static std::string create(runtimeConfEntry *conf, std::string uds_dir,int qe_pid, int session_id, int ccnt, int uid, int gid,int procid, int dbid, std::string ownername);
    static int start(std::string id, std::string& result);
    static int remove(std::vector<std::string>& ids, std::string& result);
    static int inspect_status(std::vector<std::string>& ids, std::vector<std::string>& status);
    static int mem_stats(std::vector<std::string>& ids, std::vector<std::int64_t>& mem_usage);
    static JSON_VAL get_volumes(JSON_DOC& param, runtimeConfEntry *conf, std::string uds_dir, bool& has_error);
};
#endif //__PLC_DOCKER_H__