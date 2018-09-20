#include <atomic>
#include <memory>
#include <thread>

#include <gflags/gflags.h>

#include "grpcpp/completion_queue.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/impl/codegen/async_generic_service.h"
#include "grpcpp/server_builder.h"

#include "grpcpp/generic/generic_stub.h"

using namespace grpc;

char *message_buffer;
DEFINE_int64(message_size, 100 * 1024 * 1024, "message size sent to peer");
DEFINE_int64(total_message, 1000, "tatal messages client sent");
DEFINE_int32(echo_steps, 1000, "steps to echo the progress");
DEFINE_int32(client_concurrent, 1, "client concurrently sent messages");
DEFINE_int32(port, 50051, "listen port or connecting port");
DEFINE_string(target_ip, "127.0.0.1", "the target ip client connected to");
DEFINE_string(job_type, "server", "job type, should be client/server");

DEFINE_int32(server_threads, 2, "completion threads for server");

void fill_buffer() {
  for (int i = 0; i < FLAGS_message_size; ++i)
    message_buffer[i] = 'A';
}

class AsyncService : public Service {
public:
  AsyncService() {
    AddMethod(new ::grpc::internal::RpcServiceMethod(
        "sample", ::grpc::internal::RpcMethod::NORMAL_RPC, nullptr));
    ::grpc::Service::MarkMethodAsync(0);
  }
  using Service::RequestAsyncUnary;
};

class ServerImpl final {
public:
  ServerImpl() : slice_(message_buffer, FLAGS_message_size) {}

  ~ServerImpl() {
    server_->Shutdown();
    for (int i = 0; i < FLAGS_server_threads; ++i) {
      cqs_[i]->Shutdown();
    }
  }

  void Run() {
    ServerBuilder builder;
    builder.AddListeningPort(std::string("0.0.0.0:") +
                                 std::to_string(FLAGS_port),
                             grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(-1);
    builder.SetMaxReceiveMessageSize(-1);
    builder.RegisterService(&service_);

    for (int i = 0; i < FLAGS_server_threads; ++i) {
      cqs_.emplace_back(builder.AddCompletionQueue());
    }

    server_ = builder.BuildAndStart();
    printf("server listening at 0.0.0.0:%d\n", FLAGS_port);

    for (int i = 0; i < FLAGS_server_threads; ++i) {
      threads_.emplace_back(new std::thread(
          std::bind(&ServerImpl::HandleRpcs, this, cqs_[i].get())));
    }
    for (int i = 0; i < FLAGS_server_threads; ++i) {
      threads_[i]->join();
    }
  }

  const Slice *TheSlice() const { return &slice_; }

  void AddHandler(ServerCompletionQueue *cq) {
    RpcCall *c = new RpcCall(this, &service_, cq);
    c->RegisterCall();
  }

  void HandleRpcs(ServerCompletionQueue *cq) {
    AddHandler(cq);
    void *tag;
    bool ok;
    while (true) {
      GPR_ASSERT(cq->Next(&tag, &ok));
      AddHandler(cq);
      static_cast<RpcCall *>(tag)->Response();
    }
  }

  struct RpcCall {
    RpcCall(ServerImpl *super, AsyncService *s, ServerCompletionQueue *q)
        : super_(super), svc_(s), q_(q), ctx_(),
          response_(super->TheSlice(),
                    1), // create bytebuffer with slice to prevent memory copy
          stream_(&ctx_),
          has_responsed_(false) {}

    void RegisterCall() {
      svc_->RequestAsyncUnary<ByteBuffer>(0, &ctx_, &msg_, &stream_, q_, q_,
                                          this);
    }

    void Response() {
      if (has_responsed_) {
        delete this;
      } else {
        has_responsed_ = true;
        stream_.Finish(response_, Status::OK, (void *)(this));
      }
    }

    ServerImpl *super_;
    AsyncService *svc_;
    ServerCompletionQueue *q_;
    ServerContext ctx_;
    ByteBuffer msg_;
    ByteBuffer response_;
    ServerAsyncResponseWriter<ByteBuffer> stream_;
    bool has_responsed_;
  };

private:
  std::vector<std::unique_ptr<ServerCompletionQueue>> cqs_;
  std::vector<std::unique_ptr<std::thread>> threads_;
  std::unique_ptr<Server> server_;
  AsyncService service_;
  grpc::Slice slice_;
};

class ClientImpl final {
public:
  ClientImpl() : slice_(message_buffer, FLAGS_message_size), total_count(0) {}

  bool SendRequest() {
    int count = total_count.fetch_add(1, std::memory_order_acquire);
    if (count >= FLAGS_total_message) {
      return false;
    }
    ClientRpcCall *client_call = new ClientRpcCall();
    // send a byte buffer created with slice to prevent memory copy
    client_call->response_reader = stub->PrepareUnaryCall(
        &client_call->ctx, "sample", ByteBuffer(&slice_, 1), &cq_);
    client_call->response_reader->StartCall();
    client_call->response_reader->Finish(&client_call->response_data,
                                         &client_call->response_status,
                                         client_call);
    if ((count + 1) % FLAGS_echo_steps == 0) {
      printf("has sent %d messages\n", count + 1);
    }
    return true;
  }

  void HandleResponse() {
    void *got_tag;
    bool ok;

    while (cq_.Next(&got_tag, &ok)) {
      ClientRpcCall *client_call = static_cast<ClientRpcCall *>(got_tag);
      GPR_ASSERT(ok);

      if (client_call->response_status.ok()) {
        if (!SendRequest())
          break;
      } else {
        printf("rpc call got error: %s\n",
               client_call->response_status.error_details().c_str());
      }
      delete client_call;
    }
  }

  void Run() {
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxSendMessageSize(-1);
    ch_args.SetMaxReceiveMessageSize(-1);
    std::shared_ptr<Channel> ch = grpc::CreateCustomChannel(
        FLAGS_target_ip + ":" + std::to_string(FLAGS_port),
        grpc::InsecureChannelCredentials(), ch_args);
    stub.reset(new GenericStub(ch));

    SendRequest();
    HandleResponse();

    cq_.Shutdown();
  }

  struct ClientRpcCall {
    ClientContext ctx;
    std::unique_ptr<GenericClientAsyncResponseReader> response_reader;
    ByteBuffer response_data;
    Status response_status;
  };

private:
  Slice slice_;
  CompletionQueue cq_;
  std::unique_ptr<GenericStub> stub;
  std::atomic_int total_count;
};

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  message_buffer = (char *)malloc(FLAGS_message_size);
  fill_buffer();

  if (FLAGS_job_type == "server") {
    ServerImpl server;
    server.Run();
  } else {
    ClientImpl client;
    client.Run();
  }
}
