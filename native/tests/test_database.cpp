#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>

#include "../src/db/Database.h"
#include "../src/util/Uuid.h"

using namespace sdrscan;

namespace {
std::string tempDbPath() {
    return (std::filesystem::temp_directory_path() / (makeUuid() + ".db")).string();
}
} // namespace

TEST_CASE("Database persists channel config across reopen") {
    std::string path = tempDbPath();

    ChannelConfig cc;
    cc.id = makeUuid();
    cc.freq_hz = 146'520'000;
    cc.label = "Test Channel";
    cc.mode = ChannelMode::NFM;
    cc.squelchThreshold = -62.5;
    cc.ctcssToneHz = 100.0;
    cc.dwellTime_s = 4.5;
    cc.audioGain_dB = 3.0;
    cc.mute = true;
    cc.solo = true;
    cc.hold = false;

    {
        Database db(path);
        db.initSchema();
        db.upsertChannel(cc);
    }

    {
        // Reopen - proves settings survive a process restart, the core "database instead of
        // YAML" requirement.
        Database db(path);
        db.initSchema();
        auto channels = db.listChannels();
        REQUIRE(channels.size() == 1);
        const auto& loaded = channels[0];
        CHECK(loaded.id == cc.id);
        CHECK(loaded.freq_hz == cc.freq_hz);
        CHECK(loaded.label == cc.label);
        CHECK(loaded.mode == ChannelMode::NFM);
        CHECK(loaded.squelchThreshold == cc.squelchThreshold);
        REQUIRE(loaded.ctcssToneHz.has_value());
        CHECK(*loaded.ctcssToneHz == cc.ctcssToneHz);
        CHECK(loaded.dwellTime_s == cc.dwellTime_s);
        CHECK(loaded.mute == true);
        REQUIRE(loaded.solo.has_value());
        CHECK(*loaded.solo == true);
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("Database clears CTCSS tone when set back to nullopt") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();

    ChannelConfig cc;
    cc.id = makeUuid();
    cc.freq_hz = 462'562'500;
    cc.ctcssToneHz = 123.0;
    db.upsertChannel(cc);

    cc.ctcssToneHz.reset();
    db.upsertChannel(cc);

    auto channels = db.listChannels();
    REQUIRE(channels.size() == 1);
    CHECK_FALSE(channels[0].ctcssToneHz.has_value());

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("Database round-trips receiver gains map") {
    std::string path = tempDbPath();
    Database db(path);
    db.initSchema();

    ReceiverConfig rc;
    rc.id = makeUuid();
    rc.type = ReceiverType::SOAPY;
    rc.driver = "airspy";
    rc.gains = {{"LNA", 10.0}, {"MIX", 8.0}, {"VGA", 12.0}};
    db.upsertReceiver(rc);

    auto receivers = db.listReceivers();
    REQUIRE(receivers.size() == 1);
    CHECK(receivers[0].driver.value() == "airspy");
    REQUIRE(receivers[0].gains.size() == 3);
    CHECK(receivers[0].gains.at("LNA") == 10.0);

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
