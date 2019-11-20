#ifndef __ASYNC_SERVER_H__
#define __ASYNC_SERVER_H__

#include <memory>
#include <string>

#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include "plcontainer.grpc.pb.h"
#include "docker/docker_client.h"
#include "interface.h"

extern "C"
{
#include "postgres.h"
#include "mb/pg_wchar.h"
#include "common/comm_dummy.h"
#include "plc/containers.h"
#include "cdb/cdbvars.h"
#include "utils/guc.h"
#include "plc/plc_coordinator.h"
}

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::CompletionQueue;
using grpc::Status;

using namespace plcontainer;

class AsyncServer final {
public:
    ~AsyncServer();

    void Init(const std::string &uds);    
    void Start();
    void ProcessRequest(int timeout_seconds);

private:
    PLCoordinator::AsyncService service_;
    std::unique_ptr<Server> server_;
    std::unique_ptr<ServerCompletionQueue> cq_;
};

#endif
