cmake_minimum_required(VERSION 2.8.8)

include_directories(${CMAKE_SOURCE_DIR}/grpc/output/include)
include_directories(${CMAKE_SOURCE_DIR}/grpc/third_party/gflags/build/output/include)

link_directories(${CMAKE_SOURCE_DIR}/grpc/output/lib)
link_directories(${CMAKE_SOURCE_DIR}/grpc/third_party/gflags/build/output/lib)

add_compile_options(-g)
add_compile_options(-O2)
add_compile_options(-std=c++11)

add_executable(grpc_raw grpc_raw.cc)
target_link_libraries(grpc_raw
  gflags grpc
  grpc++ pthread)

add_executable(socket_raw socket_raw.cc)
target_link_libraries(socket_raw pthread gflags)
