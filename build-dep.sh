#!/bin/bash

function build_grpc() {
    # 1. download grpc
    # some submodules of grpc is cloned from google's repo, so please 
    # make sure you can visit google when calling this command
    if [ ! -d grpc ]; then
        git clone https://github.com/grpc/grpc.git --recursive
    fi

    # 2. cd grpc
    cd grpc

    # 3. build/install grpc to `pwd`/output
    sed -i "/^prefix/c prefix\ \?\=\ $(pwd)\/output" Makefile
    rm -rf output
    make clean
    make -j8
    make install

    # 4. create symbolic link for some libraries
    ln -s $(pwd)/output/lib/libgrpc++.so $(pwd)/output/lib/libgrpc++.so.1
    
    # 5. change to parent dir
    cd ..
}

# we use gflags from grpc, so please make sure build_grpc succeed first
function build_gflags() {
    pushd grpc/third_party/gflags > /dev/null
    rm -rf build output
    mkdir build
    
    pushd build > /dev/null
    cmake .. -DCMAKE_INSTALL_PREFIX=`pwd`/output
    make -j8
    make install
    popd

    popd
}

echo "build grpc..."
build_grpc
echo "build gflags..."
build_gflags

