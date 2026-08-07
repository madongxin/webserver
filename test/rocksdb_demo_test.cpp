/**
 * @file rocksdb_demo_test.cpp
 * @brief RocksDB DemoKvStore 冒烟测试：Save -> Load -> Remove
 *
 * 用法：./rocksdb_demo_test [db_path]
 * 默认使用 /tmp/gamemesh_rocksdb_demo_test
 */

#include "DemoKvStore.h"
#include "Logging.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int64_t NowUnixSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char *argv[]) {
    Logger::setLogLevel(Logger::INFO);

    const std::string db_path =
        (argc >= 2) ? argv[1] : "/tmp/gamemesh_rocksdb_demo_test";

    auto &store = DemoKvStore::Instance();
    if (!store.InitWithPath(db_path, true)) {
        std::fprintf(stderr, "FAIL: InitWithPath %s\n", db_path.c_str());
        return 1;
    }

    kvdemo::PlayerSnapshot snap;
    snap.set_player_id(10001);
    snap.set_nickname("rocks_demo");
    snap.set_level(42);
    snap.set_updated_at_sec(NowUnixSec());
    (*snap.mutable_attrs())["atk"] = 100;
    (*snap.mutable_attrs())["def"] = 50;

    if (!store.Save(snap)) {
        std::fprintf(stderr, "FAIL: Save\n");
        return 1;
    }

    kvdemo::PlayerSnapshot loaded;
    if (!store.Load(10001, &loaded)) {
        std::fprintf(stderr, "FAIL: Load\n");
        return 1;
    }
    if (loaded.player_id() != 10001 || loaded.nickname() != "rocks_demo" ||
        loaded.level() != 42 || loaded.attrs().at("atk") != 100 ||
        loaded.attrs().at("def") != 50) {
        std::fprintf(stderr, "FAIL: field mismatch after Load\n");
        return 1;
    }
    std::printf("OK: Save/Load player_id=%llu nickname=%s level=%d\n",
                static_cast<unsigned long long>(loaded.player_id()), loaded.nickname().c_str(),
                loaded.level());

    if (!store.Remove(10001)) {
        std::fprintf(stderr, "FAIL: Remove\n");
        return 1;
    }
    kvdemo::PlayerSnapshot gone;
    if (store.Load(10001, &gone)) {
        std::fprintf(stderr, "FAIL: Load after Remove should fail\n");
        return 1;
    }
    std::printf("OK: Remove then Load miss\n");
    std::printf("PASS rocksdb_demo_test path=%s\n", db_path.c_str());
    return 0;
}
