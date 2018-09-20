#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gflags/gflags.h>

using namespace std;

char *request_buffer;
char *response_buffer;

DEFINE_int32(echo_steps, 1000, "steps to echo progress");
DEFINE_int32(port, 40041, "listening or connecting port");
DEFINE_int32(request_size, 100 * 1024 * 1024, "message client send to server");
DEFINE_int32(response_size, 100, "message server reply to client");
DEFINE_string(job_type, "server", "job type, should be client/server");

void initialize_message_buffer() {
  request_buffer = new char[FLAGS_request_size];
  for (int i = 0; i < FLAGS_request_size; ++i)
    request_buffer[i] = rand() % 128;

  response_buffer = new char[FLAGS_response_size];
  for (int i = 0; i < FLAGS_response_size; ++i)
    response_buffer[i] = rand() % 128;
}

void read_until(int fd, char *target, int size) {
  int ans;
  while (size > 0) {
    ans = read(fd, target, size);
    assert(ans != -1);
    target += ans;
    size -= ans;
  }
}

void write_until(int fd, char *source, int size) {
  int ans;
  while (size > 0) {
    ans = write(fd, source, size);
    assert(ans != -1);
    source += ans;
    size -= ans;
  }
}

void handle_socket(int socket_fd) {
  while (true) {
    read_until(socket_fd, request_buffer, FLAGS_request_size);
    write_until(socket_fd, response_buffer, FLAGS_response_size);
  }
}

void server_start() {
  int result;

  int server_socket = socket(PF_INET, SOCK_STREAM, 0);
  assert(server_socket != -1);

  struct sockaddr_in server_address;

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(FLAGS_port);

  result = bind(server_socket, (struct sockaddr *)&server_address,
                sizeof(server_address));
  assert(result >= 0);

  result = listen(server_socket, 5);
  assert(result >= 0);

  std::vector<std::thread> client_handlers;

  while (true) {
    struct sockaddr_in client_address;
    socklen_t client_address_size;
    int client_socket;

    client_address_size = sizeof(client_address);
    client_socket = accept(server_socket, (struct sockaddr *)&client_address,
                           &client_address_size);
    assert(client_socket > 0);

    client_handlers.emplace_back(std::thread(&handle_socket, client_socket));
  }

  for (std::thread &t : client_handlers) {
    t.join();
  }
}

void client_start() {
  int client_socket = socket(AF_INET, SOCK_STREAM, 0);
  assert(client_socket >= 0);

  struct sockaddr_in remote_address;
  memset(&remote_address, 0, sizeof(remote_address));
  remote_address.sin_family = AF_INET;
  remote_address.sin_port = htons(FLAGS_port);

  int result = inet_pton(AF_INET, "127.0.0.1", &remote_address.sin_addr);
  assert(result >= 0);

  result = connect(client_socket, (struct sockaddr *)&remote_address,
                   sizeof(remote_address));
  assert(result >= 0);

  int i = 0;
  while (true) {
    write_until(client_socket, request_buffer, FLAGS_request_size);
    read_until(client_socket, response_buffer, FLAGS_response_size);
    if (i % 1000 == 0)
      printf("sent %d messages\n", i);
    ++i;
  }
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  initialize_message_buffer();
  if (FLAGS_job_type == "client") {
    client_start();
  } else {
    server_start();
  }
}
