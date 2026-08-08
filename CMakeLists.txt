
cmake_minimum_required(VERSION 3.10)
project(GameMesh)

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
    runtime
    runtime/discovery
    apps
)

option(ENABLE_MYSQL "Build MySQL connection pool (libmysqlclient)" ON)
option(ENABLE_GAME_PROTOBUF "Game TCP gateway (protobuf)" ON)
option(ENABLE_REDIS "Game session store (hiredis)" ON)
option(ENABLE_ROCKSDB "Local RocksDB KV store (protobuf values)" OFF)
option(ENABLE_BRPC "baidu brpc mail dual-stack (MailBrpcService)" OFF)

set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "-g -Wall -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "-O0")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD} (required=${CMAKE_CXX_STANDARD_REQUIRED})")

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
    game/GameTcpGateway.cpp
    game/GatewayConnRegistry.cpp
    game/GatewayAuthPolicy.cpp
    runtime/PlayerSerialQueue.cpp
    runtime/InProcessTransport.cpp
    runtime/TcpReplySink.cpp
    runtime/GameRequestPlayerId.cpp
    runtime/GameRequestTransport.cpp
    runtime/MapPlacement.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MessageRoute.cpp)
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
  list(APPEND SRC_REDIS redis/RedisClient.cpp redis/RedisPool.cpp game/SessionStore.cpp
                game/PlacementStore.cpp)
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
              db/PlayerItemStore.cpp db/PlayerItemPersistQueue.cpp
              db/GameDbRepository.cpp db/GameDbOutbox.cpp db/AsyncMysqlGameDbRepository.cpp db/PlayerAccountStore.cpp
              runtime/discovery/NatsThinClient.cpp)
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
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/forward.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/forward.proto")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/session.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/session.proto")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/auth.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/auth.proto")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/gamelogic_rpc.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/gamelogic_rpc.proto")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/gateway_push.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/gateway_push.proto")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/brpc/gamedb.pb.cc")
    message(FATAL_ERROR "Run: protoc -I proto --cpp_out=runtime/brpc proto/gamedb.proto")
  endif()
  list(APPEND SRC_BRPC
    game/brpc/mail_brpc.pb.cc
    game/brpc/MailBrpcServiceImpl.cpp
    game/brpc/BrpcGameServer.cpp
    runtime/brpc/forward.pb.cc
    runtime/brpc/BrpcChannelManager.cpp
    runtime/brpc/BrpcTransport.cpp
    runtime/brpc/GameLogicForwardServiceImpl.cpp
    runtime/brpc/GameLogicServiceImpl.cpp
    runtime/brpc/GameLogicBrpcServer.cpp
    runtime/brpc/WorldForwardServiceImpl.cpp
    runtime/brpc/WorldBrpcServer.cpp
    runtime/brpc/session.pb.cc
    runtime/brpc/auth.pb.cc
    runtime/brpc/AuthServiceImpl.cpp
    runtime/brpc/AuthTokenStore.cpp
    runtime/brpc/SessionServiceImpl.cpp
    runtime/brpc/SessionBrpcServer.cpp
    runtime/brpc/SessionRpcClient.cpp
    runtime/brpc/GatewayAuthClients.cpp
    runtime/brpc/GatewayLoginOrchestrator.cpp
    runtime/brpc/gamelogic_rpc.pb.cc
    runtime/brpc/gateway_push.pb.cc
    runtime/brpc/GatewayPushServer.cpp
    runtime/brpc/GatewayPushClient.cpp
    runtime/brpc/GameLogicPush.cpp
    runtime/PushReplayCache.cpp
    runtime/brpc/gamedb.pb.cc
    runtime/brpc/GameDbServiceImpl.cpp
    runtime/brpc/GameDbBrpcServer.cpp
    db/BrpcGameDbRepository.cpp
    runtime/discovery/EtcdDiscovery.cpp
    runtime/discovery/IServiceRegistry.cpp)
  set(BRPC_LINK_LIBS ${BRPC_LIB} ${GFLAGS_LIB} ${LEVELDB_LIB} ${OPENSSL_SSL_LIB} ${ZLIB_LIB} dl)
  message(STATUS "brpc: include=${BRPC_INCLUDE_DIR} lib=${BRPC_LIB}")
endif()

set(SRC_BOOTSTRAP apps/ServerBootstrap.cpp runtime/ServiceHealth.cpp runtime/PasswordHash.cpp)
set(GAMEMESH_COMMON_SRCS
  ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5}
  ${SRC_DB} ${SRC_GAME} ${SRC_REDIS} ${SRC_ROCKSDB} ${SRC_BRPC} ${SRC_BOOTSTRAP})

function(gamemesh_apply_target_options tgt)
  target_include_directories(${tgt} PRIVATE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(${tgt} ${CMAKE_THREAD_LIBS_INIT} ${JSONCPP_LIBRARIES} ${OPENSSL_CRYPTO_LIB})
  if(ENABLE_MYSQL)
    target_compile_definitions(${tgt} PRIVATE WEBSERVER_ENABLE_MYSQL)
    target_include_directories(${tgt} PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(${tgt} ${MYSQL_CLIENT_LIB})
  endif()
  if(ENABLE_GAME_PROTOBUF)
    target_compile_definitions(${tgt} PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
    target_include_directories(${tgt} PRIVATE ${Protobuf_INCLUDE_DIRS} game)
    target_link_libraries(${tgt} ${Protobuf_LIBRARIES})
  endif()
  if(ENABLE_GAME_PROTOBUF AND ENABLE_REDIS)
    target_compile_definitions(${tgt} PRIVATE WEBSERVER_ENABLE_REDIS)
    target_include_directories(${tgt} PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(${tgt} ${HIREDIS_LIB})
  endif()
  if(ENABLE_ROCKSDB)
    target_compile_definitions(${tgt} PRIVATE WEBSERVER_ENABLE_ROCKSDB)
    target_include_directories(${tgt} PRIVATE ${ROCKSDB_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} rocksdb)
    target_link_libraries(${tgt} ${ROCKSDB_LIB} ${Protobuf_LIBRARIES})
  endif()
  if(ENABLE_BRPC)
    target_compile_definitions(${tgt} PRIVATE WEBSERVER_ENABLE_BRPC)
    target_include_directories(${tgt} PRIVATE ${BRPC_INCLUDE_DIR} game/brpc runtime runtime/brpc
                              runtime/discovery)
    target_link_libraries(${tgt} ${BRPC_LINK_LIBS})
  endif()
endfunction()

add_executable(server test/http_server.cpp ${GAMEMESH_COMMON_SRCS})
gamemesh_apply_target_options(server)

if(ENABLE_BRPC)
  add_executable(gateway apps/gateway_main.cpp ${GAMEMESH_COMMON_SRCS})
  add_executable(gamelogic apps/gamelogic_main.cpp ${GAMEMESH_COMMON_SRCS})
  add_executable(world apps/world_main.cpp ${GAMEMESH_COMMON_SRCS})
  add_executable(session apps/session_main.cpp ${GAMEMESH_COMMON_SRCS})
  add_executable(gamedb apps/gamedb_main.cpp ${GAMEMESH_COMMON_SRCS})
  gamemesh_apply_target_options(gateway)
  gamemesh_apply_target_options(gamelogic)
  gamemesh_apply_target_options(world)
  gamemesh_apply_target_options(session)
  gamemesh_apply_target_options(gamedb)
endif()

if(ENABLE_BRPC)
  # brpc::Server 等类型 over-aligned，需 -faligned-new
  set_source_files_properties(
    game/brpc/BrpcGameServer.cpp game/brpc/MailBrpcServiceImpl.cpp
    runtime/brpc/GameLogicBrpcServer.cpp runtime/brpc/GameLogicForwardServiceImpl.cpp
    runtime/brpc/GameLogicServiceImpl.cpp
    runtime/brpc/WorldBrpcServer.cpp runtime/brpc/WorldForwardServiceImpl.cpp
    runtime/brpc/BrpcTransport.cpp runtime/brpc/BrpcChannelManager.cpp
    runtime/brpc/SessionBrpcServer.cpp runtime/brpc/SessionServiceImpl.cpp
    runtime/brpc/SessionRpcClient.cpp
    runtime/brpc/AuthServiceImpl.cpp
    runtime/brpc/AuthTokenStore.cpp
    runtime/brpc/GatewayAuthClients.cpp
    runtime/brpc/GatewayLoginOrchestrator.cpp
    runtime/brpc/GatewayPushServer.cpp
    runtime/brpc/GatewayPushClient.cpp
    runtime/brpc/GameLogicPush.cpp
    runtime/brpc/GameDbBrpcServer.cpp runtime/brpc/GameDbServiceImpl.cpp
    db/BrpcGameDbRepository.cpp
    apps/ServerBootstrap.cpp
    PROPERTIES COMPILE_FLAGS "-faligned-new")
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

  add_executable(game_tcp_smoke_client
    client/game_tcp_smoke_client.cpp
    game/ProtoFraming.cpp
    game/game.pb.cc)
  target_include_directories(game_tcp_smoke_client PRIVATE
    ${Protobuf_INCLUDE_DIRS} game)
  target_link_libraries(game_tcp_smoke_client
    ${CMAKE_THREAD_LIBS_INIT} ${Protobuf_LIBRARIES})
endif()

# 流式文件哈希测试
add_executable(file_hash_test test/file_hash_test.cpp base/FileHash.cpp)
target_include_directories(file_hash_test PRIVATE base ${OPENSSL_INCLUDE_DIR})
target_link_libraries(file_hash_test ${OPENSSL_CRYPTO_LIB})

# 阶段 1：player 串行队列有序性
add_executable(player_serial_queue_test
  test/player_serial_queue_test.cpp
  runtime/PlayerSerialQueue.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(player_serial_queue_test PRIVATE runtime base log timer)
target_link_libraries(player_serial_queue_test ${CMAKE_THREAD_LIBS_INIT})

# 阶段 5：MapPlacement + epoch 栅栏（无 AOI/Tick）
add_executable(map_placement_test
  test/map_placement_test.cpp
  runtime/MapPlacement.cpp
  runtime/MapInstanceRegistry.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(map_placement_test PRIVATE runtime base log timer)
target_link_libraries(map_placement_test ${CMAKE_THREAD_LIBS_INIT})

# 阶段 6：消息类型 → World / Logic 路由分类
if(ENABLE_GAME_PROTOBUF)
  add_executable(message_route_test
    test/message_route_test.cpp
    runtime/MessageRoute.cpp
    game/game.pb.cc)
  target_compile_definitions(message_route_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(message_route_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game)
  target_link_libraries(message_route_test ${Protobuf_LIBRARIES})

  add_executable(auth_session_boundary_test
    test/auth_session_boundary_test.cpp
    game/GameLogic.cpp
    game/GatewayConnRegistry.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapPlacement.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(auth_session_boundary_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(auth_session_boundary_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer)
  target_link_libraries(auth_session_boundary_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(phase1_gateway_boundary_test
    test/phase1_gateway_boundary_test.cpp
    game/GatewayConnRegistry.cpp
    game/GatewayAuthPolicy.cpp
    game/ProtoFraming.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(phase1_gateway_boundary_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(phase1_gateway_boundary_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game base log timer)
  target_link_libraries(phase1_gateway_boundary_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
endif()

# 阶段 3：Session 状态机（需 Redis）
if(ENABLE_GAME_PROTOBUF AND ENABLE_REDIS)
  add_executable(session_store_test
    test/session_store_test.cpp
    game/SessionStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(session_store_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(session_store_test PRIVATE
    game redis base log timer ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
  target_link_libraries(session_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES})

  add_executable(placement_store_test
    test/placement_store_test.cpp
    game/PlacementStore.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(placement_store_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(placement_store_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(placement_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(push_replay_cache_test
    test/push_replay_cache_test.cpp
    runtime/PushReplayCache.cpp)
  target_include_directories(push_replay_cache_test PRIVATE runtime)
endif()

add_executable(password_hash_test
  test/password_hash_test.cpp
  runtime/PasswordHash.cpp)
target_include_directories(password_hash_test PRIVATE runtime ${OPENSSL_INCLUDE_DIR})
target_link_libraries(password_hash_test ${OPENSSL_CRYPTO_LIB})

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
endif()

# 阶段 0：Reactor / Buffer / ProtoFraming 单元测试
add_executable(reactor_unit_test
  test/reactor_unit_test.cpp
  tcp/Buffer.cpp
  game/ProtoFraming.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(reactor_unit_test PRIVATE tcp game base log timer)
target_link_libraries(reactor_unit_test ${CMAKE_THREAD_LIBS_INIT})

# 阶段 0：连接 ID / 发送队列 / EPOLLOUT 集成风格测试（本地 loopback）
add_executable(reactor_integration_test
  test/reactor_integration_test.cpp
  ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5})
target_include_directories(reactor_integration_test PRIVATE
  base tcp http log timer ${OPENSSL_INCLUDE_DIR})
target_link_libraries(reactor_integration_test
  ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

# 邮件领取并发集成测试（需 MySQL）
if(ENABLE_MYSQL AND ENABLE_GAME_PROTOBUF)
  set(SRC_GAMEDB_MAIL_TEST
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
    db/GameDbRepository.cpp
    db/GameDbOutbox.cpp
    db/AsyncMysqlGameDbRepository.cpp
    db/PlayerAccountStore.cpp
    runtime/discovery/NatsThinClient.cpp
    runtime/MapPlacement.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MessageRoute.cpp
    ${SRC_LIST1}
    ${SRC_LIST2}
    ${SRC_LIST3}
    ${SRC_LIST4}
    ${SRC_LIST5})

  add_executable(mail_claim_integration_test
    test/mail_claim_integration_test.cpp
    ${SRC_GAMEDB_MAIL_TEST})
  if(ENABLE_REDIS)
    target_sources(mail_claim_integration_test PRIVATE redis/RedisClient.cpp redis/RedisPool.cpp
                    game/SessionStore.cpp)
    target_compile_definitions(mail_claim_integration_test PRIVATE WEBSERVER_ENABLE_REDIS)
    target_include_directories(mail_claim_integration_test PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(mail_claim_integration_test ${HIREDIS_LIB})
  endif()
  target_compile_definitions(mail_claim_integration_test PRIVATE
    WEBSERVER_ENABLE_MYSQL WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(mail_claim_integration_test PRIVATE
    ${MYSQL_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} game db redis base tcp log timer http runtime
    runtime/discovery)
  target_include_directories(mail_claim_integration_test PRIVATE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(mail_claim_integration_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB} ${Protobuf_LIBRARIES} ${JSONCPP_LIBRARIES}
    ${OPENSSL_CRYPTO_LIB})

  add_executable(gamedb_mail_claim_test
    test/gamedb_mail_claim_test.cpp
    ${SRC_GAMEDB_MAIL_TEST})
  if(ENABLE_REDIS)
    target_sources(gamedb_mail_claim_test PRIVATE redis/RedisClient.cpp redis/RedisPool.cpp
                    game/SessionStore.cpp)
    target_compile_definitions(gamedb_mail_claim_test PRIVATE WEBSERVER_ENABLE_REDIS)
    target_include_directories(gamedb_mail_claim_test PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(gamedb_mail_claim_test ${HIREDIS_LIB})
  endif()
  target_compile_definitions(gamedb_mail_claim_test PRIVATE
    WEBSERVER_ENABLE_MYSQL WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(gamedb_mail_claim_test PRIVATE
    ${MYSQL_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} game db redis base tcp log timer http runtime
    runtime/discovery)
  target_include_directories(gamedb_mail_claim_test PRIVATE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(gamedb_mail_claim_test
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
