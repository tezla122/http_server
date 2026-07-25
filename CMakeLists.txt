cmake_minimum_required(VERSION 4.2)
project(http_server)

set(CMAKE_CXX_STANDARD 20)

add_executable(http_server main.cpp)
target_link_libraries(http_server ws2_32)

# Automatically copy the 'public' folder into the build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/public DESTINATION ${CMAKE_CURRENT_BINARY_DIR})