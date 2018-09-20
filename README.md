## rpcbench

Collection of simple programs to benchmark the throughput of rpc frameworks. Currently grpc & raw socket are supported.

## How to build

1. run "build-dep.sh" to build the dependency of grpc & gflags
2. build the soruce codes with cmake

## How to usage

### grpc_raw

```
1. start a server: ./grpc_raw --job_type=server --message_size=100
2. start a client: ./grpc_raw --job_type=client --message_size=100000000
```

in this example above: client will send message with about 100M to server, and server will reply with a response of 100B.

For more parameters of the program, you may want to refer to "grpc_raw --help"

### socket_raw

```
1. start a server: ./socket_raw --job_type=server --request_size=100000000 --response_size=100
2. start a client: ./socket_raw --job_type=client --request_size=100000000 --response_size=100
```

in this example above: client will send message with about 100M to server, and server will reply with a response of 100B.

Notice you need to assign the **same** request size and response size for both the client and server side.

For more parameters of the program, you may want to refer to "socket_raw --help"