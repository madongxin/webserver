
cmake_minimum_required(VERSION 3.10)
project(WebServer)

# 供 Cursor/VS Code（Microsoft C++、clangd）跳转定义/实现
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json for IDE" FORCE)

include_directories(
    /usr/include
    /usr/local/include
    base
    tcp
    log
    http
    timer
    db
    redis
    rocksdb
)

option(ENABLE_MYSQL "Build MySQL connection pool (libmysqlclient)" ON)
option(ENABLE_GAME_PROTOBUF "Game TCP gateway (protobuf)" ON)
option(ENABLE_REDIS "Game session store (hiredis)" ON)
option(ENABLE_ROCKSDB "Local RocksDB KV store (protobuf values)" OFF)
option(ENABLE_BRPC "baidu brpc mail dual-stack (MailBrpcService)" OFF)

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

# 流式文件哈希 FileHash（OpenSSL EVP）
find_path(OPENSSL_INCLUDE_DIR openssl/evp.h PATHS /usr/include /usr/local/include)
find_library(OPENSSL_CRYPTO_LIB NAMES crypto PATHS /usr/lib64 /usr/lib /usr/local/lib64)
if(NOT OPENSSL_INCLUDE_DIR OR NOT OPENSSL_CRYPTO_LIB)
  message(FATAL_ERROR "OpenSSL crypto not found (needed by base/FileHash). Install openssl-devel.")
endif()
message(STATUS "OpenSSL crypto: include=${OPENSSL_INCLUDE_DIR} lib=${OPENSSL_CRYPTO_LIB}")

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
  if(ENABLE_MYSQL)
    list(APPEND SRC_GAME
      game/MailTypes.cpp
      game/MailConfig.cpp
      game/MailStore.cpp
      game/MailService.cpp
      game/MailExpireScanner.cpp)
  endif()
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

set(SRC_ROCKSDB "")
if(ENABLE_ROCKSDB)
  find_package(Protobuf REQUIRED)
  find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h
            PATHS /usr/local/include /usr/include)
  find_library(ROCKSDB_LIB NAMES rocksdb
               PATHS /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
  if(NOT ROCKSDB_INCLUDE_DIR OR NOT ROCKSDB_LIB)
    message(FATAL_ERROR "ENABLE_ROCKSDB=ON but RocksDB not found. Install librocksdb (e.g. to /usr/local).")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rocksdb/kv_demo.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=rocksdb proto/kv_demo.proto")
  endif()
  list(APPEND SRC_ROCKSDB
    rocksdb/RocksDbClient.cpp
    rocksdb/DemoKvStore.cpp
    rocksdb/kv_demo.pb.cc)
  message(STATUS "RocksDB: include=${ROCKSDB_INCLUDE_DIR} lib=${ROCKSDB_LIB}")
endif()

set(SRC_BRPC "")
set(BRPC_LINK_LIBS "")
if(ENABLE_BRPC)
  if(NOT ENABLE_GAME_PROTOBUF OR NOT ENABLE_MYSQL)
    message(FATAL_ERROR "ENABLE_BRPC requires ENABLE_GAME_PROTOBUF=ON and ENABLE_MYSQL=ON")
  endif()
  find_path(BRPC_INCLUDE_DIR brpc/server.h PATHS /usr/local/include /usr/include)
  find_library(BRPC_LIB NAMES brpc PATHS /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
  find_library(GFLAGS_LIB NAMES gflags PATHS /usr/lib64 /usr/lib)
  find_library(LEVELDB_LIB NAMES leveldb PATHS /usr/lib64 /usr/lib)
  find_library(OPENSSL_SSL_LIB NAMES ssl PATHS /usr/lib64 /usr/lib)
  find_library(ZLIB_LIB NAMES z PATHS /usr/lib64 /usr/lib)
  if(NOT BRPC_INCLUDE_DIR OR NOT BRPC_LIB)
    message(FATAL_ERROR "ENABLE_BRPC=ON but brpc not found. Install to /usr/local (libbrpc).")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/brpc/mail_brpc.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=game/brpc proto/mail_brpc.proto")
  endif()
  list(APPEND SRC_BRPC
    game/brpc/mail_brpc.pb.cc
    game/brpc/MailBrpcServiceImpl.cpp
    game/brpc/BrpcGameServer.cpp)
  set(BRPC_LINK_LIBS ${BRPC_LIB} ${GFLAGS_LIB} ${LEVELDB_LIB} ${OPENSSL_SSL_LIB} ${ZLIB_LIB} dl)
  message(STATUS "brpc: include=${BRPC_INCLUDE_DIR} lib=${BRPC_LIB}")
endif()

add_executable(webserver test/http_server.cpp ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5} ${SRC_DB} ${SRC_GAME} ${SRC_REDIS} ${SRC_ROCKSDB} ${SRC_BRPC})
target_include_directories(webserver PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(webserver ${CMAKE_THREAD_LIBS_INIT} ${JSONCPP_LIBRARIES} ${OPENSSL_CRYPTO_LIB})

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

if(ENABLE_ROCKSDB)
  # RocksDB 8.x 头文件需要 C++17（全局仍为 -std=c++14，此处覆盖）
  set_source_files_properties(
    rocksdb/RocksDbClient.cpp rocksdb/DemoKvStore.cpp
    PROPERTIES COMPILE_FLAGS "-std=c++17")
  target_compile_definitions(webserver PRIVATE WEBSERVER_ENABLE_ROCKSDB)
  target_include_directories(webserver PRIVATE ${ROCKSDB_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} rocksdb)
  target_link_libraries(webserver ${ROCKSDB_LIB} ${Protobuf_LIBRARIES})
endif()

if(ENABLE_BRPC)
  # brpc::Server 等类型 over-aligned，需 -faligned-new
  set_source_files_properties(
    game/brpc/BrpcGameServer.cpp game/brpc/MailBrpcServiceImpl.cpp
    PROPERTIES COMPILE_FLAGS "-faligned-new")
  target_compile_definitions(webserver PRIVATE WEBSERVER_ENABLE_BRPC)
  target_include_directories(webserver PRIVATE ${BRPC_INCLUDE_DIR} game/brpc)
  target_link_libraries(webserver ${BRPC_LINK_LIBS})
endif()

# 邮件状态机单元测试（无第三方测试框架）
add_executable(mail_unit_test test/mail_unit_test.cpp game/MailTypes.cpp)
target_include_directories(mail_unit_test PRIVATE game base)

# brpc 邮件客户端（Login + MailList 等）
if(ENABLE_BRPC)
  add_executable(mail_brpc_client
    client/mail_brpc_client.cpp
    game/brpc/mail_brpc.pb.cc
    game/game.pb.cc)
  target_include_directories(mail_brpc_client PRIVATE
    ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} game game/brpc)
  target_link_libraries(mail_brpc_client
    ${CMAKE_THREAD_LIBS_INIT} ${Protobuf_LIBRARIES} ${BRPC_LINK_LIBS} ${OPENSSL_CRYPTO_LIB})
  target_compile_options(mail_brpc_client PRIVATE -faligned-new)
endif()

# 流式文件哈希测试
add_executable(file_hash_test test/file_hash_test.cpp base/FileHash.cpp)
target_include_directories(file_hash_test PRIVATE base ${OPENSSL_INCLUDE_DIR})
target_link_libraries(file_hash_test ${OPENSSL_CRYPTO_LIB})

# RocksDB Demo KV 测试（需 ENABLE_ROCKSDB=ON）
if(ENABLE_ROCKSDB)
  add_executable(rocksdb_demo_test
    test/rocksdb_demo_test.cpp
    rocksdb/RocksDbClient.cpp
    rocksdb/DemoKvStore.cpp
    rocksdb/kv_demo.pb.cc
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(rocksdb_demo_test PRIVATE WEBSERVER_ENABLE_ROCKSDB)
  target_include_directories(rocksdb_demo_test PRIVATE
    ${ROCKSDB_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} rocksdb base log timer)
  target_link_libraries(rocksdb_demo_test
    ${CMAKE_THREAD_LIBS_INIT} ${ROCKSDB_LIB} ${Protobuf_LIBRARIES})
  # 覆盖全局 -std=c++14（RocksDB 头文件依赖 C++17）
  target_compile_options(rocksdb_demo_test PRIVATE -std=c++17)
endif()

# 邮件领取并发集成测试（需 MySQL）
if(ENABLE_MYSQL AND ENABLE_GAME_PROTOBUF)
  add_executable(mail_claim_integration_test
    test/mail_claim_integration_test.cpp
    game/MailTypes.cpp
    game/MailConfig.cpp
    game/MailStore.cpp
    game/MailService.cpp
    game/GameLogic.cpp
    game/game.pb.cc
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/PlayerItemStore.cpp
    db/PlayerItemPersistQueue.cpp
    ${SRC_LIST1}
    ${SRC_LIST2}
    ${SRC_LIST3}
    ${SRC_LIST4}
    ${SRC_LIST5})
  if(ENABLE_REDIS)
    target_sources(mail_claim_integration_test PRIVATE redis/RedisClient.cpp game/SessionStore.cpp)
    target_compile_definitions(mail_claim_integration_test PRIVATE WEBSERVER_ENABLE_REDIS)
    target_include_directories(mail_claim_integration_test PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(mail_claim_integration_test ${HIREDIS_LIB})
  endif()
  target_compile_definitions(mail_claim_integration_test PRIVATE
    WEBSERVER_ENABLE_MYSQL WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(mail_claim_integration_test PRIVATE
    ${MYSQL_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} game db redis base tcp log timer http)
  target_include_directories(mail_claim_integration_test PRIVATE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(mail_claim_integration_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB} ${Protobuf_LIBRARIES} ${JSONCPP_LIBRARIES}
    ${OPENSSL_CRYPTO_LIB})
endif()

# 每次 build 时同步到项目根，便于 clangd 默认扫描 ${workspaceFolder}
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  add_custom_target(ide_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COMMENT "Sync compile_commands.json to project root for IDE")
endif()
