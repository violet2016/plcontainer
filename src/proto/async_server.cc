#include "async_server.h"

// Base class used to cast the void* tags we get from the completion queue and call Proceed() on them.
class Call {
public:
    virtual void Proceed(bool ok) = 0;
};

class StartContainerCall final : public Call {
public:
    explicit StartContainerCall(PLCoordinator::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : service_(service), cq_(cq), responder_(&ctx_), status_(REQUEST) {
        service_->RequestStartContainer(&ctx_, &request_, &responder_, cq_, cq_, this);
    }

    void Proceed(bool ok) {
        int ret;
        switch (status_) {
        case REQUEST:
            new StartContainerCall(service_, cq_);
            if (!ok) {
                responder_.FinishWithError(grpc::Status::CANCELLED, this);
                plc_elog(WARNING, "StartContainer request is not ok. Finishing.");
            } else {
                char *uds_address;
                char *container_id;
                char *log_msg;
                ret = start_container(request_.runtime_id().c_str(), (pid_t)request_.qe_pid(), request_.session_id(), request_.command_count(), &uds_address, &container_id, &log_msg);
                if (ret == 0) {
                    response_.set_container_address(uds_address);
                    response_.set_container_id(container_id);
                }
                response_.set_status(ret);
                response_.set_log_msg(log_msg);
                responder_.Finish(response_, grpc::Status::OK, this);
                pfree(uds_address);
                pfree(container_id);
                pfree(log_msg);
                plc_elog(DEBUG1, "StartContainer request successfully. request:%s response:%s",
                        request_.DebugString().c_str(),
                        response_.DebugString().c_str());
            }
            status_ = FINISH;
            break;

        case FINISH:
            if (!ok) {
                plc_elog(ERROR, "StartContainer RPC finished unexpectedly");
            }
            delete this;
            break;
        }
    }

private:
    PLCoordinator::AsyncService* service_;
    grpc::ServerCompletionQueue* cq_;
    grpc::ServerContext ctx_;
    grpc::ServerAsyncResponseWriter<StartContainerResponse> responder_;
    StartContainerRequest request_;
    StartContainerResponse response_;
    enum CallStatus { REQUEST, FINISH };
    CallStatus status_;
};

class StopContainerCall final : public Call {
public:
    explicit StopContainerCall(PLCoordinator::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : service_(service), cq_(cq), responder_(&ctx_), status_(REQUEST) {
        service_->RequestStopContainer(&ctx_, &request_, &responder_, cq_, cq_, this);
    }

    void Proceed(bool ok) {
        switch (status_) {
        case REQUEST:
            new StopContainerCall(service_, cq_);
            if (!ok) {
                responder_.FinishWithError(grpc::Status::CANCELLED, this);
                plc_elog(WARNING, "StopContainer Request is not ok. Finishing.");
            } else {
                response_.set_status(destroy_container((pid_t)request_.qe_pid(), request_.session_id(), request_.command_count()));
                responder_.Finish(response_, grpc::Status::OK, this);
                plc_elog(DEBUG1, "StopContainer request successfully. request:%s response:%s",
                        request_.DebugString().c_str(),
                        response_.DebugString().c_str());
            }
            status_ = FINISH;
            break;

        case FINISH:
            if (!ok) {
                plc_elog(ERROR, "StopContainer RPC finished unexpectedly");
            }
            delete this;
            break;
        }
    }

private:
    PLCoordinator::AsyncService* service_;
    grpc::ServerCompletionQueue* cq_;
    grpc::ServerContext ctx_;
    grpc::ServerAsyncResponseWriter<StopContainerResponse> responder_;
    StopContainerRequest request_;
    StopContainerResponse response_;
    enum CallStatus { REQUEST, FINISH };
    CallStatus status_;
};

AsyncServer::~AsyncServer() {
    server_->Shutdown();
    cq_->Shutdown();
}

void AsyncServer::Init(const std::string &uds) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("unix://"+uds, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    cq_ = builder.AddCompletionQueue();
    server_ = builder.BuildAndStart();
    plc_elog(LOG, "Asynchronous server listening on address %s.", uds.c_str());
}

void AsyncServer::Start() {
    new StartContainerCall(&service_, cq_.get());
    new StopContainerCall(&service_, cq_.get());
    plc_elog(LOG, "Asynchronous server started.");
}

void AsyncServer::ProcessRequest(int timeout_seconds) {
    void* tag;
    bool ok;
    CompletionQueue::NextStatus status; 
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
    for (;;) {
        status = cq_->AsyncNext(&tag, &ok, deadline);
        if (status == CompletionQueue::GOT_EVENT) {
            plc_elog(LOG, "aserver got request event to handle, ok:%d", ok);
            static_cast<Call*>(tag)->Proceed(ok);
            continue;
        } else if (status == CompletionQueue::TIMEOUT) {
            plc_elog(DEBUG1, "aserver got timeout event, return to caller, ok:%d", ok);
            break;
        } else if (status == CompletionQueue::SHUTDOWN) {
            plc_elog(LOG, "server got shutdown event, return to caller, ok:%d", ok);
            break;
        } else {
            plc_elog(ERROR, "server got unknown status:%d", status);
            break;
        }
    }
}

PLCoordinatorServer *start_server(const char *address) {
    PLCoordinatorServer *server = (PLCoordinatorServer *)palloc(sizeof(PLCoordinatorServer));
    server->address = pstrdup(address);
    server->server = new AsyncServer;
    plc_elog(LOG, "start_server with server:%p address:%s", server->server, server->address);
    ((AsyncServer *)server->server)->Init(address);
    ((AsyncServer *)server->server)->Start();
    return server;
}

int process_request(PLCoordinatorServer *server, int timeout_seconds) {
    AsyncServer *s = (AsyncServer *)server->server;
    s->ProcessRequest(timeout_seconds);
    plc_elog(DEBUG1, "server %p timeout in %d seconds", server, timeout_seconds);
    return 0;
}
