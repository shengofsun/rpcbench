#include <gflags/gflags.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "grpcpp/completion_queue.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/impl/codegen/async_generic_service.h"
#include "grpcpp/server_builder.h"

#include "grpcpp/generic/generic_stub.h"

using namespace grpc;

char *message_buffer;
DEFINE_int64(message_size, 100 * 1024 * 1024, "message size sent to peer");
DEFINE_int64(total_message, 1000, "tatal messages client sent");
DEFINE_int64(report_interval_secs, 2,
             "report statistics infos interval seconds");
DEFINE_int32(client_channels, 1, "opening channels for client to send");
DEFINE_int32(client_channel_concurrent, 1,
             "client concurrently sent messages for each channels");

DEFINE_int32(port, 50051, "listen port or connecting port");
DEFINE_string(target_ip, "127.0.0.1", "the target ip client connected to");
DEFINE_string(job_type, "server", "job type, should be client/server");

DEFINE_int32(server_threads, 2, "completion threads for server");

void fill_buffer() {
  for (int i = 0; i < FLAGS_message_size; ++i) message_buffer[i] = 'A';
}

long gettid() { return syscall(SYS_gettid); }

void log_message(const char *msg) {
  int srclen = strlen(msg);
  do {
    int ans = write(1, msg, srclen);
    srclen -= ans;
    msg += ans;
  } while (srclen > 0);
}

uint64_t NowNanos() {
  auto now = std::chrono::high_resolution_clock::now();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
                   .count();
  return nanos;
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
    builder.AddListeningPort(
        std::string("0.0.0.0:") + std::to_string(FLAGS_port),
        grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(-1);
    builder.SetMaxReceiveMessageSize(-1);
    builder.RegisterService(&service_);

    for (int i = 0; i < FLAGS_server_threads; ++i) {
      cqs_.emplace_back(builder.AddCompletionQueue());
    }

    server_ = builder.BuildAndStart();
    char buffer[128];
    snprintf(buffer, 127, "server listening at 0.0.0.0:%d\n", FLAGS_port);
    log_message(buffer);

    for (int i = 0; i < FLAGS_server_threads; ++i) {
      threads_.emplace_back(new std::thread(
          std::bind(&ServerImpl::HandleRpcs, this, cqs_[i].get())));
    }
    reporter_.reset(new std::thread(std::bind(&ServerImpl::Report, this)));

    for (int i = 0; i < FLAGS_server_threads; ++i) {
      threads_[i]->join();
    }
    reporter_->join();
  }

  const Slice *TheSlice() const { return &slice_; }

  void AddHandler(ServerCompletionQueue *cq) {
    RpcCall *c = new RpcCall(this, &service_, cq);
    c->RegisterCall();
  }

  void HandleRpcs(ServerCompletionQueue *cq) {
    char buffer[128];
    snprintf(buffer, 127, "handle rpc with thread(%ld)\n", gettid());
    log_message(buffer);
    AddHandler(cq);
    void *tag;
    bool ok;
    while (true) {
      GPR_ASSERT(cq->Next(&tag, &ok));
      AddHandler(cq);
      static_cast<RpcCall *>(tag)->Response();
    }
  }

  void Report() {
    uint64_t n = NowNanos();
    uint64_t recved = 0;
    uint64_t responsed = 0;

    while (true) {
      std::this_thread::sleep_for(
          std::chrono::seconds(FLAGS_report_interval_secs));

      uint64_t new_tick = NowNanos();
      uint64_t new_recved = recved_bytes_.load(std::memory_order_acquire);
      uint64_t new_responsed = responsed_bytes_.load(std::memory_order_acquire);

      double recv_gbps = (new_recved - recved) * 8.0 / (new_tick - n);
      double resp_gbps = (new_responsed - responsed) * 8.0 / (new_tick - n);

      n = new_tick;
      recved = new_recved;
      responsed = new_responsed;
      printf("recved Gbps: %lf, responsed Gbps: %lf\n", recv_gbps, resp_gbps);
    }
  }

  struct RpcCall {
    RpcCall(ServerImpl *super, AsyncService *s, ServerCompletionQueue *q)
        : super_(super),
          svc_(s),
          q_(q),
          ctx_(),
          // create bytebuffer with slice to prevent memory copy
          response_(super->TheSlice(), 1),
          stream_(&ctx_),
          has_responsed_(false) {}

    void RegisterCall() {
      svc_->RequestAsyncUnary<ByteBuffer>(0, &ctx_, &msg_, &stream_, q_, q_,
                                          this);
    }

    void Response() {
      if (has_responsed_) {
        super_->responsed_bytes_.fetch_add(response_.Length(),
                                           std::memory_order_relaxed);
        delete this;
      } else {
        has_responsed_ = true;
        super_->recved_bytes_.fetch_add(msg_.Length(),
                                        std::memory_order_relaxed);
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

  std::unique_ptr<std::thread> reporter_;
  std::atomic_uint64_t recved_bytes_{0};
  std::atomic_uint64_t responsed_bytes_{0};
};

class ClientImpl final {
 public:
  ClientImpl() : slice_(message_buffer, FLAGS_message_size), total_count(0) {}

  bool SendRequest(int worker_index) {
    int count = total_count.fetch_add(1, std::memory_order_acquire);
    if (count >= FLAGS_total_message) {
      return false;
    }
    ClientRpcCall *client_call = new ClientRpcCall();
    // send a byte buffer created with slice to prevent memory copy
    client_call->response_reader = stubs_[worker_index]->PrepareUnaryCall(
        &client_call->ctx, "sample", ByteBuffer(&slice_, 1),
        cqs_[worker_index].get());
    client_call->response_reader->StartCall();
    client_call->response_reader->Finish(&client_call->response_data,
                                         &client_call->response_status,
                                         client_call);
    ++send_count_[worker_index];
    return true;
  }

  void HandleResponse(int worker_index) {
    void *got_tag;
    bool ok;

    while (cqs_[worker_index]->Next(&got_tag, &ok)) {
      ClientRpcCall *client_call = static_cast<ClientRpcCall *>(got_tag);
      GPR_ASSERT(ok);

      ++responsed_count_[worker_index];
      if (client_call->response_status.ok()) {
        total_sent_bytes_.fetch_add(slice_.size(), std::memory_order_relaxed);
        total_responsed_bytes_.fetch_add(client_call->response_data.Length(),
                                         std::memory_order_relaxed);
        if (!SendRequest(worker_index) &&
            send_count_[worker_index] <= responsed_count_[worker_index])
          break;
      } else {
        printf("rpc call got error: %s\n",
               client_call->response_status.error_details().c_str());
      }
      delete client_call;
    }
  }

  void WorkerRun(int worker_index) {
    char buffer[128];
    snprintf(buffer, 127, "run worker with thread(%ld)\n", gettid());
    log_message(buffer);

    for (int i = 0; i < FLAGS_client_channel_concurrent; ++i) {
      SendRequest(worker_index);
    }

    HandleResponse(worker_index);
  }

  void Run() {
    for (int i = 0; i < FLAGS_client_channels; ++i) {
      grpc::ChannelArguments ch_args;
      ch_args.SetMaxSendMessageSize(-1);
      ch_args.SetMaxReceiveMessageSize(-1);
      std::shared_ptr<Channel> ch = grpc::CreateCustomChannel(
          FLAGS_target_ip + ":" + std::to_string(FLAGS_port),
          grpc::InsecureChannelCredentials(), ch_args);
      stubs_.emplace_back(new GenericStub(ch));

      cqs_.emplace_back(new CompletionQueue());
    }

    send_count_.resize(FLAGS_client_channels);
    responsed_count_.resize(FLAGS_client_channels);

    reporter_.reset(new std::thread(std::bind(&ClientImpl::Report, this)));

    uint64_t t = NowNanos();
    for (int i = 0; i < FLAGS_client_channels; ++i) {
      workers_.emplace_back(
          std::thread(std::bind(&ClientImpl::WorkerRun, this, i)));
    }

    for (int i = 0; i < FLAGS_client_channels; ++i) {
      workers_[i].join();
      cqs_[i]->Shutdown();
    }
    t = NowNanos() - t;
    all_finished.store(true, std::memory_order_release);

    reporter_->join();
    double seconds = (t + .0) / 1000000000;
    printf("total handled messages: %ld, throughput: %lf bps\n",
           FLAGS_total_message,
           FLAGS_total_message * FLAGS_message_size * 8 / seconds);
  }

  void Report() {
    uint64_t n = NowNanos();
    uint64_t sent = 0;
    uint64_t responsed = 0;

    while (!all_finished.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(
          std::chrono::seconds(FLAGS_report_interval_secs));

      uint64_t new_tick = NowNanos();
      uint64_t new_sent = total_sent_bytes_.load(std::memory_order_acquire);
      uint64_t new_responsed =
          total_responsed_bytes_.load(std::memory_order_acquire);

      double sent_gbps = (new_sent - sent) * 8.0 / (new_tick - n);
      double resp_gbps = (new_responsed - responsed) * 8.0 / (new_tick - n);

      n = new_tick;
      sent = new_sent;
      responsed = new_responsed;
      printf("sent Gbps: %lf, responsed Gbps: %lf\n", sent_gbps, resp_gbps);
    }
  }

  struct ClientRpcCall {
    ClientContext ctx;
    std::unique_ptr<GenericClientAsyncResponseReader> response_reader;
    ByteBuffer response_data;
    Status response_status;
  };

 private:
  Slice slice_;
  std::vector<std::unique_ptr<CompletionQueue>> cqs_;
  std::vector<std::unique_ptr<GenericStub>> stubs_;
  std::vector<std::thread> workers_;
  std::vector<int> send_count_;
  std::vector<int> responsed_count_;
  std::atomic_int total_count;

  std::atomic_uint64_t total_sent_bytes_{0};
  std::atomic_uint64_t total_responsed_bytes_{0};
  std::atomic_bool all_finished{false};
  std::unique_ptr<std::thread> reporter_;
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
