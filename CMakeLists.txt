
cmake_minimum_required(VERSION 3.10)
project(WebServer)

include_directories(
    /usr/include
    base
    tcp
    log
    http
    timer
    db
    redis
)

option(ENABLE_MYSQL "Build MySQL connection pool (libmysqlclient)" ON)
option(ENABLE_GAME_PROTOBUF "Game TCP gateway (protobuf)" ON)
option(ENABLE_REDIS "Game session store (hiredis)" ON)

set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++14")
set(CMAKE_CXX_FLAGS_DEBUG "-O0")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/build/test/)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP jsoncpp)
link_libraries(${JSONCPP_LIBRARIES})

aux_source_directory(base SRC_LIST1)
aux_source_directory(tcp SRC_LIST2)
aux_source_directory(http SRC_LIST3)
aux_source_directory(timer SRC_LIST4)
aux_source_directory(log SRC_LIST5)

set(SRC_GAME "")
if(ENABLE_GAME_PROTOBUF)
  find_package(Protobuf REQUIRED)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/game.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=game proto/game.proto")
  endif()
  list(APPEND SRC_GAME
    game/game.pb.cc
    game/ProtoFraming.cpp
    game/GameLogic.cpp
    game/GameService.cpp
    game/GameTcpGateway.cpp)
  message(STATUS "Game protobuf: ${Protobuf_LIBRARIES}")
endif()

set(SRC_REDIS "")
if(ENABLE_GAME_PROTOBUF AND ENABLE_REDIS)
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include)
  find_library(HIREDIS_LIB NAMES hiredis PATHS /usr/lib64 /usr/lib)
  if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
    message(FATAL_ERROR "ENABLE_REDIS=ON but hiredis not found. Install hiredis-devel.")
  endif()
  list(APPEND SRC_REDIS redis/RedisClient.cpp game/SessionStore.cpp)
  message(STATUS "Redis: include=${HIREDIS_INCLUDE_DIR} lib=${HIREDIS_LIB}")
endif()

set(SRC_DB "")
if(ENABLE_MYSQL)
  find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
            PATHS /usr/include /usr/local/include /usr/include/mysql)
  find_library(MYSQL_CLIENT_LIB NAMES mysqlclient
                PATHS /usr/lib64/mysql /usr/lib/mysql /usr/lib64 /usr/lib)
  if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_CLIENT_LIB)
    message(FATAL_ERROR "ENABLE_MYSQL=ON but mysql client not found.")
  endif()
  list(APPEND SRC_DB db/Connection.cpp db/ConnectionPool.cpp db/MetricsDbWriter.cpp
              db/PlayerItemStore.cpp db/PlayerItemPersistQueue.cpp)
  message(STATUS "MySQL pool: include=${MYSQL_INCLUDE_DIR} lib=${MYSQL_CLIENT_LIB}")
endif()

add_executable(webserver test/http_server.cpp ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5} ${SRC_DB} ${SRC_GAME} ${SRC_REDIS})
target_link_libraries(webserver ${CMAKE_THREAD_LIBS_INIT} ${JSONCPP_LIBRARIES})

if(ENABLE_MYSQL)
  target_compile_definitions(webserver PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(webserver PRIVATE ${MYSQL_INCLUDE_DIR})
  target_link_libraries(webserver ${MYSQL_CLIENT_LIB})
endif()

if(ENABLE_GAME_PROTOBUF)
  target_compile_definitions(webserver PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(webserver PRIVATE ${Protobuf_INCLUDE_DIRS} game)
  target_link_libraries(webserver ${Protobuf_LIBRARIES})
endif()

if(ENABLE_GAME_PROTOBUF AND ENABLE_REDIS)
  target_compile_definitions(webserver PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(webserver PRIVATE ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(webserver ${HIREDIS_LIB})
endif()
