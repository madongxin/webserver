
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

file(SHA256 "${CMAKE_SOURCE_DIR}/proto/game.proto" GAMEMESH_SCHEMA_SHA256)
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GAMEMESH_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT GAMEMESH_GIT_SHA)
  set(GAMEMESH_GIT_SHA "unknown")
endif()
add_compile_definitions(
  GAMEMESH_SCHEMA_SHA256="${GAMEMESH_SCHEMA_SHA256}"
  GAMEMESH_GIT_SHA="${GAMEMESH_GIT_SHA}")
message(STATUS "protocol schema_sha256=${GAMEMESH_SCHEMA_SHA256}")
message(STATUS "git sha=${GAMEMESH_GIT_SHA}")

set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "-g -Wall -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "-O0")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD} (required=${CMAKE_CXX_STANDARD_REQUIRED})")

# Threads 探测必须在 -fsanitize=* 之前，否则 TSan 会让 FindThreads 失败
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP jsoncpp)
link_libraries(${JSONCPP_LIBRARIES})

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()
if(ENABLE_ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()
if(ENABLE_UBSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
endif()
if(ENABLE_TSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

# 随 -B 构建目录变化，避免 build-asan 等污染默认 build/test
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/test/)

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
    game/CommandPolicy.cpp
    runtime/PlayerSerialQueue.cpp
    runtime/GatewayDisconnectAsync.cpp
    runtime/InProcessTransport.cpp
    runtime/TcpReplySink.cpp
    runtime/PublicError.cpp
    runtime/ProtocolHandshake.cpp
    runtime/GatewayConnGuard.cpp
    runtime/GameRequestPlayerId.cpp
    runtime/GameRequestTransport.cpp
    runtime/MapPlacement.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapLeaseKeeper.cpp
    runtime/PlacementRecoveryScheduler.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MessageRoute.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp)
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
                game/PlacementStore.cpp game/PushReplayStore.cpp
                runtime/SecureRandom.cpp runtime/HealthyLogicSnapshot.cpp)
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
              db/GameDbRepository.cpp db/GameDbOutbox.cpp db/AsyncMysqlGameDbRepository.cpp
              db/PlayerAccountStore.cpp db/PlayerProfileStore.cpp db/PlayerProfileDefaults.cpp
              db/LastSafePositionStore.cpp
              db/GameDbAssetStore.cpp
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
    runtime/AuthLoginRateLimit.cpp
    runtime/brpc/SessionServiceImpl.cpp
    runtime/brpc/SessionBrpcServer.cpp
    runtime/brpc/SessionRpcClient.cpp
    runtime/brpc/GatewayAuthClients.cpp
    runtime/brpc/GatewayLoginOrchestrator.cpp
    runtime/brpc/GatewayEnterMapOrchestrator.cpp
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
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/RedisServiceRegistry.cpp
    runtime/HealthyLogicOwners.cpp)
  set(BRPC_LINK_LIBS ${BRPC_LIB} ${GFLAGS_LIB} ${LEVELDB_LIB} ${OPENSSL_SSL_LIB} ${ZLIB_LIB} dl)
  message(STATUS "brpc: include=${BRPC_INCLUDE_DIR} lib=${BRPC_LIB}")
endif()

set(SRC_BOOTSTRAP apps/ServerBootstrap.cpp runtime/ServiceHealth.cpp runtime/PasswordHash.cpp
    runtime/GatewayIdentity.cpp runtime/GatewayAuthFlow.cpp runtime/OpsMetrics.cpp
    runtime/HealthDeps.cpp runtime/HealthProbe.cpp runtime/RpcOffloadPool.cpp)
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
    runtime/brpc/GatewayEnterMapOrchestrator.cpp
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

  add_executable(game_tcp_e2e_client
    client/game_tcp_e2e_client.cpp
    game/ProtoFraming.cpp
    game/game.pb.cc)
  target_include_directories(game_tcp_e2e_client PRIVATE
    ${Protobuf_INCLUDE_DIRS} game)
  target_link_libraries(game_tcp_e2e_client
    ${CMAKE_THREAD_LIBS_INIT} ${Protobuf_LIBRARIES})
endif()

if(ENABLE_REDIS AND ENABLE_GAME_PROTOBUF)
  add_executable(placement_seed_tool
    test/placement_seed_tool.cpp
    game/PlacementStore.cpp
    game/SessionStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/SecureRandom.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(placement_seed_tool PRIVATE
    WEBSERVER_ENABLE_REDIS WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(placement_seed_tool PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(placement_seed_tool
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB})
endif()

# 流式文件哈希测试
add_executable(file_hash_test test/file_hash_test.cpp base/FileHash.cpp)
target_include_directories(file_hash_test PRIVATE base ${OPENSSL_INCLUDE_DIR})
target_link_libraries(file_hash_test ${OPENSSL_CRYPTO_LIB})

add_executable(utf8_text_test test/utf8_text_test.cpp)
target_include_directories(utf8_text_test PRIVATE base runtime)

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

add_executable(player_serial_shutdown_test
  test/player_serial_shutdown_test.cpp
  runtime/PlayerSerialQueue.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(player_serial_shutdown_test PRIVATE runtime base log timer)
target_link_libraries(player_serial_shutdown_test ${CMAKE_THREAD_LIBS_INIT})

add_executable(secure_random_test
  test/secure_random_test.cpp
  runtime/SecureRandom.cpp)
target_include_directories(secure_random_test PRIVATE runtime ${OPENSSL_INCLUDE_DIR})
target_link_libraries(secure_random_test ${OPENSSL_CRYPTO_LIB})

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

add_executable(map_static_data_test
  test/map_static_data_test.cpp
  runtime/MapStaticData.cpp
  runtime/MapCatalog.cpp
  base/FileHash.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(map_static_data_test PRIVATE
  runtime base log timer ${OPENSSL_INCLUDE_DIR} ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(map_static_data_test
  ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

add_executable(map_runtime_aoi_test
  test/map_runtime_aoi_test.cpp
  runtime/MapRuntime.cpp
  runtime/MapStaticData.cpp
  runtime/MapCatalog.cpp
  base/FileHash.cpp
  base/CurrentThread.cpp
  log/Logging.cpp
  log/LogStream.cpp
  log/LogFile.cpp
  log/AsyncLogging.cpp)
target_include_directories(map_runtime_aoi_test PRIVATE
  runtime base log timer ${OPENSSL_INCLUDE_DIR} ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(map_runtime_aoi_test
  ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

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
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/FileHash.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(auth_session_boundary_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(auth_session_boundary_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(auth_session_boundary_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
                        ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

  add_executable(asset_dirty_consistency_test
    test/asset_dirty_consistency_test.cpp
    game/GameLogic.cpp
    game/GatewayConnRegistry.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapPlacement.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/FileHash.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(asset_dirty_consistency_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(asset_dirty_consistency_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(asset_dirty_consistency_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
                        ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

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

  # 不启用 WEBSERVER_ENABLE_BRPC：避免拉齐 SessionRpcClient/Push 全依赖；Channel 持有测见下方可选 target
  add_executable(phase1_correctness_test
    test/phase1_correctness_test.cpp
    game/GameLogic.cpp
    game/GatewayConnRegistry.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapPlacement.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/PlayerSerialQueue.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/FileHash.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(phase1_correctness_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(phase1_correctness_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(phase1_correctness_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
                        ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

  add_executable(trusted_player_id_test
    test/trusted_player_id_test.cpp
    game/game.pb.cc)
  target_compile_definitions(trusted_player_id_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(trusted_player_id_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} game)
  target_link_libraries(trusted_player_id_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(placement_authority_test
    test/placement_authority_test.cpp)
  target_include_directories(placement_authority_test PRIVATE game)
  # header-only ValidateAuthorityWrite；无需 Redis 运行时

  add_executable(placement_formal_fence_test
    test/placement_formal_fence_test.cpp)
  target_include_directories(placement_formal_fence_test PRIVATE game)

  add_executable(command_policy_test
    test/command_policy_test.cpp
    game/CommandPolicy.cpp
    game/game.pb.cc)
  target_compile_definitions(command_policy_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(command_policy_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} game runtime)
  target_link_libraries(command_policy_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(client_protocol_test
    test/client_protocol_test.cpp
    game/game.pb.cc)
  target_compile_definitions(client_protocol_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(client_protocol_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} game)
  target_link_libraries(client_protocol_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(protocol_compat_test
    test/protocol_compat_test.cpp)
  target_include_directories(protocol_compat_test PRIVATE ${Protobuf_INCLUDE_DIRS})
  target_link_libraries(protocol_compat_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(protocol_handshake_test
    test/protocol_handshake_test.cpp
    runtime/ProtocolHandshake.cpp
    runtime/PublicError.cpp
    runtime/OpsMetrics.cpp
    game/ProtoFraming.cpp
    game/game.pb.cc)
  target_compile_definitions(protocol_handshake_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(protocol_handshake_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} game runtime)
  target_link_libraries(protocol_handshake_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

  add_executable(gateway_overload_test
    test/gateway_overload_test.cpp
    runtime/PlayerSerialQueue.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_include_directories(gateway_overload_test PRIVATE runtime base log timer)
  target_link_libraries(gateway_overload_test ${CMAKE_THREAD_LIBS_INIT})

  add_executable(phase2_transfer_snapshot_test
    test/phase2_transfer_snapshot_test.cpp
    game/GameLogic.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapPlacement.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/FileHash.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(phase2_transfer_snapshot_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(phase2_transfer_snapshot_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(phase2_transfer_snapshot_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
                        ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

  add_executable(world_snapshot_test
    test/world_snapshot_test.cpp
    game/GameLogic.cpp
    game/game.pb.cc
    runtime/MessageRoute.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapPlacement.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/discovery/IServiceRegistry.cpp
    runtime/discovery/EtcdDiscovery.cpp
    base/FileHash.cpp
    base/LogicMetrics.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(world_snapshot_test PRIVATE WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(world_snapshot_test PRIVATE
    ${Protobuf_INCLUDE_DIRS} runtime game runtime/discovery base log timer ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(world_snapshot_test ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
                        ${OPENSSL_CRYPTO_LIB} ${JSONCPP_LIBRARIES})

  if(ENABLE_BRPC)
    add_executable(phase1_channel_hold_test
      test/phase1_channel_hold_test.cpp
      runtime/brpc/BrpcChannelManager.cpp
      base/CurrentThread.cpp
      log/Logging.cpp
      log/LogStream.cpp
      log/LogFile.cpp
      log/AsyncLogging.cpp)
    target_include_directories(phase1_channel_hold_test PRIVATE
      runtime runtime/brpc base log timer ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(phase1_channel_hold_test
      ${BRPC_LINK_LIBS} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB} ${CMAKE_THREAD_LIBS_INIT})

    add_executable(channel_snapshot_race_test
      test/channel_snapshot_race_test.cpp
      runtime/brpc/BrpcChannelManager.cpp
      base/CurrentThread.cpp
      log/Logging.cpp
      log/LogStream.cpp
      log/LogFile.cpp
      log/AsyncLogging.cpp)
    target_include_directories(channel_snapshot_race_test PRIVATE
      runtime runtime/brpc base log timer ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(channel_snapshot_race_test
      ${BRPC_LINK_LIBS} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB} ${CMAKE_THREAD_LIBS_INIT})

    add_executable(gateway_disconnect_async_test
      test/gateway_disconnect_async_test.cpp
      runtime/GatewayDisconnectAsync.cpp
      runtime/brpc/SessionRpcClient.cpp
      runtime/brpc/GatewayAuthClients.cpp
      runtime/brpc/session.pb.cc
      runtime/brpc/auth.pb.cc
      runtime/brpc/gamelogic_rpc.pb.cc
      game/game.pb.cc
      base/CurrentThread.cpp
      log/Logging.cpp
      log/LogStream.cpp
      log/LogFile.cpp
      log/AsyncLogging.cpp)
    target_compile_definitions(gateway_disconnect_async_test PRIVATE
      WEBSERVER_ENABLE_BRPC WEBSERVER_ENABLE_GAME_PROTOBUF)
    target_include_directories(gateway_disconnect_async_test PRIVATE
      runtime runtime/brpc game base log timer
      ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(gateway_disconnect_async_test
      ${BRPC_LINK_LIBS} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB} ${CMAKE_THREAD_LIBS_INIT})

    add_executable(gamedb_rpc_tool
      test/gamedb_rpc_tool.cpp
      runtime/brpc/gamedb.pb.cc)
    target_include_directories(gamedb_rpc_tool PRIVATE
      runtime/brpc ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(gamedb_rpc_tool
      ${BRPC_LINK_LIBS} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB} ${CMAKE_THREAD_LIBS_INIT})

    add_executable(logic_dispatch_tool
      test/logic_dispatch_tool.cpp
      runtime/brpc/gamelogic_rpc.pb.cc)
    target_include_directories(logic_dispatch_tool PRIVATE
      runtime/brpc ${BRPC_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(logic_dispatch_tool
      ${BRPC_LINK_LIBS} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB} ${CMAKE_THREAD_LIBS_INIT})
  endif()

  add_executable(player_serial_async_test
    test/player_serial_async_test.cpp
    runtime/PlayerSerialQueue.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_include_directories(player_serial_async_test PRIVATE runtime base log timer)
  target_link_libraries(player_serial_async_test ${CMAKE_THREAD_LIBS_INIT})

  add_executable(gateway_conn_race_test
    test/gateway_conn_race_test.cpp
    game/GatewayConnRegistry.cpp
    runtime/GatewayAuthFlow.cpp
    runtime/GatewayIdentity.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_include_directories(gateway_conn_race_test PRIVATE runtime game base log timer)
  target_link_libraries(gateway_conn_race_test ${CMAKE_THREAD_LIBS_INIT})

  add_executable(gateway_logout_policy_test
    test/gateway_logout_policy_test.cpp)
  target_include_directories(gateway_logout_policy_test PRIVATE runtime)
  target_link_libraries(gateway_logout_policy_test ${CMAKE_THREAD_LIBS_INIT})

  add_executable(discovery_ha_test
    test/discovery_ha_test.cpp
    runtime/discovery/IServiceRegistry.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_include_directories(discovery_ha_test PRIVATE runtime runtime/brpc runtime/discovery base log
                                                      timer)
  target_link_libraries(discovery_ha_test ${CMAKE_THREAD_LIBS_INIT})
endif()

# 阶段 3：Session 状态机（需 Redis）
if(ENABLE_GAME_PROTOBUF AND ENABLE_REDIS)
  add_executable(session_store_test
    test/session_store_test.cpp
    game/SessionStore.cpp
    game/PlacementStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/SecureRandom.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(session_store_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(session_store_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR})

  add_executable(player_transfer_test
    test/player_transfer_test.cpp
    game/SessionStore.cpp
    game/PlacementStore.cpp
    game/GatewayConnRegistry.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/SecureRandom.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(player_transfer_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(player_transfer_test PRIVATE
    game redis runtime base log timer ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(player_transfer_test
    ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_CRYPTO_LIB})
  target_link_libraries(session_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB})

  add_executable(placement_store_test
    test/placement_store_test.cpp
    game/PlacementStore.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/HealthyLogicSnapshot.cpp
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

  add_executable(map_occupancy_test
    test/map_occupancy_test.cpp
    game/PlacementStore.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(map_occupancy_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(map_occupancy_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(map_occupancy_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(placement_recovery_test
    test/placement_recovery_test.cpp
    game/PlacementStore.cpp
    runtime/PlacementRecoveryScheduler.cpp
    runtime/OpsMetrics.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(placement_recovery_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(placement_recovery_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(placement_recovery_test ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(phase3_discovery_test
    test/phase3_discovery_test.cpp
    runtime/discovery/RedisServiceRegistry.cpp
    runtime/discovery/IServiceRegistry.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(phase3_discovery_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(phase3_discovery_test PRIVATE
    redis base log timer runtime runtime/discovery ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(phase3_discovery_test ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(healthy_logic_refresh_test
    test/healthy_logic_refresh_test.cpp
    runtime/HealthyLogicOwners.cpp
    runtime/HealthyLogicSnapshot.cpp
    runtime/SecureRandom.cpp
    runtime/OpsMetrics.cpp
    runtime/discovery/RedisServiceRegistry.cpp
    runtime/discovery/IServiceRegistry.cpp
    game/SessionStore.cpp
    game/PlacementStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(healthy_logic_refresh_test PRIVATE WEBSERVER_ENABLE_REDIS
                            WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(healthy_logic_refresh_test PRIVATE
    game redis base log timer runtime runtime/discovery ${HIREDIS_INCLUDE_DIR}
    ${Protobuf_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(healthy_logic_refresh_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB})

  add_executable(session_grace_cas_test
    test/session_grace_cas_test.cpp
    game/SessionStore.cpp
    game/PlacementStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/SecureRandom.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(session_grace_cas_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(session_grace_cas_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(session_grace_cas_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB})

  add_executable(reconnect_transaction_test
    test/reconnect_transaction_test.cpp
    game/SessionStore.cpp
    game/PlacementStore.cpp
    game/game.pb.cc
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/SecureRandom.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(reconnect_transaction_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(reconnect_transaction_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(reconnect_transaction_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES} ${OPENSSL_CRYPTO_LIB})

  add_executable(redis_pool_shutdown_test
    test/redis_pool_shutdown_test.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(redis_pool_shutdown_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(redis_pool_shutdown_test PRIVATE
    redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(redis_pool_shutdown_test ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(map_lease_drill
    test/map_lease_drill.cpp
    game/PlacementStore.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    runtime/HealthyLogicSnapshot.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(map_lease_drill PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(map_lease_drill PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(map_lease_drill ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(push_replay_store_test
    test/push_replay_store_test.cpp
    game/PushReplayStore.cpp
    runtime/PushReplayCache.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(push_replay_store_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(push_replay_store_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(push_replay_store_test ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(push_ack_redis_test
    test/push_ack_redis_test.cpp
    game/PushReplayStore.cpp
    runtime/PushReplayCache.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(push_ack_redis_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(push_ack_redis_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR})
  target_link_libraries(push_ack_redis_test ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB})

  add_executable(push_full_snapshot_test
    test/push_full_snapshot_test.cpp
    game/PushReplayStore.cpp
    runtime/PushReplayCache.cpp
    redis/RedisClient.cpp
    redis/RedisPool.cpp
    game/game.pb.cc
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(push_full_snapshot_test PRIVATE
    WEBSERVER_ENABLE_REDIS WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(push_full_snapshot_test PRIVATE
    game redis base log timer runtime ${HIREDIS_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})
  target_link_libraries(push_full_snapshot_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${Protobuf_LIBRARIES})

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

add_executable(service_health_test
  test/service_health_test.cpp
  runtime/ServiceHealth.cpp
  runtime/HealthDeps.cpp
  runtime/HealthProbe.cpp)
target_include_directories(service_health_test PRIVATE runtime)
target_link_libraries(service_health_test ${CMAKE_THREAD_LIBS_INIT})

# 阶段 6：正式模式 MySQL 边界（ForbidInit + FormalModeAllowsMysql）
if(ENABLE_MYSQL)
  add_executable(formal_mysql_boundary_test
    test/formal_mysql_boundary_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(formal_mysql_boundary_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(formal_mysql_boundary_test PRIVATE
    db runtime base log timer ${MYSQL_INCLUDE_DIR})
  target_link_libraries(formal_mysql_boundary_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})
endif()

if(ENABLE_REDIS)
  add_executable(auth_token_store_test
    test/auth_token_store_test.cpp
    runtime/brpc/AuthTokenStore.cpp
    runtime/SecureRandom.cpp
    runtime/PasswordHash.cpp
    redis/RedisClient.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(auth_token_store_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(auth_token_store_test PRIVATE
    runtime runtime/brpc redis base log timer ${HIREDIS_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(auth_token_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${OPENSSL_CRYPTO_LIB})

  add_executable(refresh_token_race_test
    test/refresh_token_race_test.cpp
    runtime/brpc/AuthTokenStore.cpp
    runtime/SecureRandom.cpp
    runtime/PasswordHash.cpp
    redis/RedisClient.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(refresh_token_race_test PRIVATE WEBSERVER_ENABLE_REDIS)
  target_include_directories(refresh_token_race_test PRIVATE
    runtime runtime/brpc redis base log timer ${HIREDIS_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(refresh_token_race_test
    ${CMAKE_THREAD_LIBS_INIT} ${HIREDIS_LIB} ${OPENSSL_CRYPTO_LIB})
endif()

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
  runtime/OpsMetrics.cpp
  ${SRC_LIST1} ${SRC_LIST2} ${SRC_LIST3} ${SRC_LIST4} ${SRC_LIST5})
target_include_directories(reactor_integration_test PRIVATE
  base tcp http log timer runtime ${OPENSSL_INCLUDE_DIR})
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
    db/PlayerProfileStore.cpp
    db/PlayerProfileDefaults.cpp
    db/LastSafePositionStore.cpp
    db/GameDbAssetStore.cpp
    runtime/OpsMetrics.cpp
    runtime/PlayerSerialQueue.cpp
    runtime/discovery/NatsThinClient.cpp
    runtime/MapPlacement.cpp
    runtime/MapInstanceRegistry.cpp
    runtime/MapStaticData.cpp
    runtime/MapCatalog.cpp
    runtime/MapRuntime.cpp
    runtime/ForwardMetaContext.cpp
    runtime/MessageRoute.cpp
    ${SRC_LIST1}
    ${SRC_LIST2}
    ${SRC_LIST3}
    ${SRC_LIST4}
    ${SRC_LIST5})

  add_executable(phase3_gamedb_asset_test
    test/phase3_gamedb_asset_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/GameDbOutbox.cpp
    db/GameDbAssetStore.cpp
    db/PlayerItemStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(phase3_gamedb_asset_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(phase3_gamedb_asset_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer)
  target_link_libraries(phase3_gamedb_asset_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(gamedb_snapshot_idempotency_test
    test/gamedb_snapshot_idempotency_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/GameDbOutbox.cpp
    db/GameDbAssetStore.cpp
    db/PlayerItemStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(gamedb_snapshot_idempotency_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(gamedb_snapshot_idempotency_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer)
  target_link_libraries(gamedb_snapshot_idempotency_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(gamedb_mutation_idempotency_test
    test/gamedb_mutation_idempotency_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/GameDbOutbox.cpp
    db/GameDbAssetStore.cpp
    db/PlayerItemStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(gamedb_mutation_idempotency_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(gamedb_mutation_idempotency_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer)
  target_link_libraries(gamedb_mutation_idempotency_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(gamedb_mutation_atomicity_test
    test/gamedb_mutation_atomicity_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/GameDbOutbox.cpp
    db/GameDbAssetStore.cpp
    db/PlayerItemStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(gamedb_mutation_atomicity_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(gamedb_mutation_atomicity_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer)
  target_link_libraries(gamedb_mutation_atomicity_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(gamedb_unknown_result_test
    test/gamedb_unknown_result_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/GameDbOutbox.cpp
    db/GameDbAssetStore.cpp
    db/PlayerItemStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(gamedb_unknown_result_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(gamedb_unknown_result_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer)
  target_link_libraries(gamedb_unknown_result_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(mail_claim_integration_test
    test/mail_claim_integration_test.cpp
    ${SRC_GAMEDB_MAIL_TEST})
  if(ENABLE_REDIS)
    target_sources(mail_claim_integration_test PRIVATE redis/RedisClient.cpp redis/RedisPool.cpp
                    game/SessionStore.cpp game/PlacementStore.cpp game/PushReplayStore.cpp
                    runtime/SecureRandom.cpp runtime/HealthyLogicSnapshot.cpp)
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
                    game/SessionStore.cpp game/PlacementStore.cpp game/PushReplayStore.cpp
                    runtime/SecureRandom.cpp runtime/HealthyLogicSnapshot.cpp)
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

  add_executable(player_profile_store_test
    test/player_profile_store_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/PlayerAccountStore.cpp
    db/PlayerProfileStore.cpp
    db/PlayerProfileDefaults.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(player_profile_store_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(player_profile_store_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer game)
  target_link_libraries(player_profile_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(last_safe_position_store_test
    test/last_safe_position_store_test.cpp
    db/Connection.cpp
    db/ConnectionPool.cpp
    db/LastSafePositionStore.cpp
    base/CurrentThread.cpp
    log/Logging.cpp
    log/LogStream.cpp
    log/LogFile.cpp
    log/AsyncLogging.cpp)
  target_compile_definitions(last_safe_position_store_test PRIVATE WEBSERVER_ENABLE_MYSQL)
  target_include_directories(last_safe_position_store_test PRIVATE
    ${MYSQL_INCLUDE_DIR} db base log timer runtime)
  target_link_libraries(last_safe_position_store_test
    ${CMAKE_THREAD_LIBS_INIT} ${MYSQL_CLIENT_LIB})

  add_executable(player_mail_send_test
    test/player_mail_send_test.cpp
    ${SRC_GAMEDB_MAIL_TEST})
  if(ENABLE_REDIS)
    target_sources(player_mail_send_test PRIVATE redis/RedisClient.cpp redis/RedisPool.cpp
                    game/SessionStore.cpp game/PlacementStore.cpp game/PushReplayStore.cpp
                    runtime/SecureRandom.cpp runtime/HealthyLogicSnapshot.cpp)
    target_compile_definitions(player_mail_send_test PRIVATE WEBSERVER_ENABLE_REDIS)
    target_include_directories(player_mail_send_test PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(player_mail_send_test ${HIREDIS_LIB})
  endif()
  target_compile_definitions(player_mail_send_test PRIVATE
    WEBSERVER_ENABLE_MYSQL WEBSERVER_ENABLE_GAME_PROTOBUF)
  target_include_directories(player_mail_send_test PRIVATE
    ${MYSQL_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS} game db redis base tcp log timer http runtime
    runtime/discovery)
  target_include_directories(player_mail_send_test PRIVATE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(player_mail_send_test
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
