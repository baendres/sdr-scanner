// Tests for Scanner's config-editing methods added for the settings-page web UI: channel
// structural edits (frequency/label/mode) and receiver/output CRUD. These are all safe to
// exercise without starting the scanner (no SDR hardware needed) - loadConfigFromDatabase()
// doesn't require any receivers, and buildWindows() (which editChannel triggers) early-returns
// when none are configured yet, same as before start() is ever called.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>

#include "../src/db/Database.h"
#include "../src/scanner/Scanner.h"
#include "../src/util/Uuid.h"

using namespace sdrscan;

namespace {
std::string tempDbPath() {
    return (std::filesystem::temp_directory_path() / (makeUuid() + ".db")).string();
}
} // namespace

TEST_CASE("Scanner::editChannel updates frequency/label/mode and persists them") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();
    Scanner scanner(db);
    scanner.loadConfigFromDatabase();

    ChannelConfig cc;
    cc.freq_hz = 146'520'000;
    cc.label = "Original";
    cc.mode = ChannelMode::FM;
    std::string id = scanner.addChannel(cc);

    auto snap1 = scanner.getSnapshot();
    REQUIRE(snap1.channels.size() == 1);
    CHECK(snap1.channels[0].freq_hz == 146'520'000);

    ChannelConfig updated = snap1.channels[0];
    updated.freq_hz = 462'562'500;
    updated.label = "Renamed";
    updated.mode = ChannelMode::NFM;
    scanner.editChannel(updated);

    auto snap2 = scanner.getSnapshot();
    REQUIRE(snap2.channels.size() == 1);
    CHECK(snap2.channels[0].id == id);
    CHECK(snap2.channels[0].freq_hz == 462'562'500);
    CHECK(snap2.channels[0].label == "Renamed");
    CHECK(snap2.channels[0].mode == ChannelMode::NFM);

    // Persisted, not just in-memory - reopen against the same file and re-load.
    Database db2(path);
    Scanner scanner2(db2);
    scanner2.loadConfigFromDatabase();
    auto snap3 = scanner2.getSnapshot();
    REQUIRE(snap3.channels.size() == 1);
    CHECK(snap3.channels[0].freq_hz == 462'562'500);
    CHECK(snap3.channels[0].label == "Renamed");

    std::filesystem::remove(path);
}

TEST_CASE("Scanner::editChannel rejects an unknown channel id") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();
    Scanner scanner(db);
    scanner.loadConfigFromDatabase();

    ChannelConfig cc;
    cc.id = makeUuid();
    cc.freq_hz = 100'000'000;
    CHECK_THROWS_AS(scanner.editChannel(cc), std::runtime_error);

    std::filesystem::remove(path);
}

TEST_CASE("Scanner receiver config CRUD assigns an id, persists, and deletes") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();
    Scanner scanner(db);
    scanner.loadConfigFromDatabase();

    ReceiverConfig rc;
    rc.type = ReceiverType::RTL_SDR;
    rc.gain = 20.0;
    std::string id = scanner.upsertReceiverConfig(rc);
    REQUIRE(!id.empty());

    auto snap = scanner.getSnapshot();
    REQUIRE(snap.receivers.size() == 1);
    CHECK(snap.receivers[0].id == id);
    CHECK(snap.receivers[0].gain.has_value());
    CHECK(*snap.receivers[0].gain == 20.0);

    // Edit: same id, different gain.
    ReceiverConfig updated = snap.receivers[0];
    updated.gain = 30.0;
    std::string id2 = scanner.upsertReceiverConfig(updated);
    CHECK(id2 == id);

    auto snap2 = scanner.getSnapshot();
    REQUIRE(snap2.receivers.size() == 1); // still one row, not a duplicate
    CHECK(*snap2.receivers[0].gain == 30.0);

    scanner.deleteReceiverConfig(id);
    auto snap3 = scanner.getSnapshot();
    CHECK(snap3.receivers.empty());

    std::filesystem::remove(path);
}

TEST_CASE("Scanner output config CRUD assigns an id, persists, and deletes") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();
    Scanner scanner(db);
    scanner.loadConfigFromDatabase();

    OutputConfig oc;
    oc.type = "udp";
    oc.configJson = R"({"serverIp":"127.0.0.1","serverPort":12345})";
    int64_t id = scanner.upsertOutputConfig(oc);
    CHECK(id != 0);

    auto snap = scanner.getSnapshot();
    REQUIRE(snap.outputs.size() == 1);
    CHECK(snap.outputs[0].id == id);
    CHECK(snap.outputs[0].type == "udp");

    OutputConfig updated = snap.outputs[0];
    updated.configJson = R"({"serverIp":"127.0.0.1","serverPort":54321})";
    int64_t id2 = scanner.upsertOutputConfig(updated);
    CHECK(id2 == id);

    auto snap2 = scanner.getSnapshot();
    REQUIRE(snap2.outputs.size() == 1); // still one row, not a duplicate
    CHECK(snap2.outputs[0].configJson.find("54321") != std::string::npos);

    scanner.deleteOutputConfig(id);
    auto snap3 = scanner.getSnapshot();
    CHECK(snap3.outputs.empty());

    std::filesystem::remove(path);
}
