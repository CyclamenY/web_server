cmake_minimum_required(VERSION 3.15)
project(TinyWebServer)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread -lmysqlclient")

add_executable(TinyWebServer
        main.cpp
        ./CGImysql/sqlConnectionPool.cpp
        ./http/httpConn.cpp
        ./log/log.cpp
        ./timer/listTimer.cpp
        ./config/config.cpp
        ./webserver/webServer.cpp
        string/stringFunc.cpp)

target_link_libraries(TinyWebServer mysqlclient)