#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

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
    cc.squelchNoiseMargin_dB = 8.0;
    cc.noiseSquelchThreshold_dB = 12.0;
    cc.dmrSlot = 2;
    cc.dmrTalkgroupFilter = 31337;
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
        REQUIRE(loaded.squelchNoiseMargin_dB.has_value());
        CHECK(*loaded.squelchNoiseMargin_dB == cc.squelchNoiseMargin_dB);
        REQUIRE(loaded.noiseSquelchThreshold_dB.has_value());
        CHECK(*loaded.noiseSquelchThreshold_dB == cc.noiseSquelchThreshold_dB);
        REQUIRE(loaded.dmrSlot.has_value());
        CHECK(*loaded.dmrSlot == cc.dmrSlot);
        REQUIRE(loaded.dmrTalkgroupFilter.has_value());
        CHECK(*loaded.dmrTalkgroupFilter == cc.dmrTalkgroupFilter);
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

TEST_CASE("Database migrates an existing channels table that predates squelch_noise_margin_db") {
    // Simulates a database created before this column existed (e.g. an already-deployed
    // instance) - initSchema()'s CREATE TABLE IF NOT EXISTS alone wouldn't touch it, so this
    // proves the explicit ALTER TABLE migration path actually runs and old rows survive.
    std::string path = tempDbPath();
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        const char* oldSchema = R"SQL(
            CREATE TABLE channels (
                id TEXT PRIMARY KEY, freq_hz INTEGER NOT NULL, label TEXT,
                mode TEXT NOT NULL DEFAULT 'FM', audio_gain_db REAL NOT NULL DEFAULT 0,
                dwell_time_s REAL NOT NULL DEFAULT 3.0, squelch_threshold REAL NOT NULL DEFAULT -55,
                ctcss_tone_hz REAL, enabled INTEGER NOT NULL DEFAULT 1, disable_until REAL,
                mute INTEGER NOT NULL DEFAULT 0, solo INTEGER, hold INTEGER NOT NULL DEFAULT 0,
                sort_order INTEGER NOT NULL DEFAULT 0
            );
            INSERT INTO channels(id, freq_hz, label) VALUES ('pre-existing', 462562500, 'Old Row');
        )SQL";
        REQUIRE(sqlite3_exec(raw, oldSchema, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }

    Database db(path);
    db.initSchema();

    auto channels = db.listChannels();
    REQUIRE(channels.size() == 1);
    CHECK(channels[0].id == "pre-existing");
    CHECK_FALSE(channels[0].squelchNoiseMargin_dB.has_value());

    ChannelConfig cc = channels[0];
    cc.squelchNoiseMargin_dB = 5.0;
    db.upsertChannel(cc);
    channels = db.listChannels();
    REQUIRE(channels[0].squelchNoiseMargin_dB.has_value());
    CHECK(*channels[0].squelchNoiseMargin_dB == 5.0);

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("Database migrates an existing channels table that predates noise_squelch_threshold_db") {
    // Same situation as the squelch_noise_margin_db migration test above, for the newer
    // noise_squelch_threshold_db column - a database created before this column existed needs
    // the explicit ALTER TABLE path, not just CREATE TABLE IF NOT EXISTS.
    std::string path = tempDbPath();
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        const char* oldSchema = R"SQL(
            CREATE TABLE channels (
                id TEXT PRIMARY KEY, freq_hz INTEGER NOT NULL, label TEXT,
                mode TEXT NOT NULL DEFAULT 'FM', audio_gain_db REAL NOT NULL DEFAULT 0,
                dwell_time_s REAL NOT NULL DEFAULT 3.0, squelch_threshold REAL NOT NULL DEFAULT -55,
                ctcss_tone_hz REAL, squelch_noise_margin_db REAL,
                enabled INTEGER NOT NULL DEFAULT 1, disable_until REAL,
                mute INTEGER NOT NULL DEFAULT 0, solo INTEGER, hold INTEGER NOT NULL DEFAULT 0,
                sort_order INTEGER NOT NULL DEFAULT 0
            );
            INSERT INTO channels(id, freq_hz, label) VALUES ('pre-existing', 462562500, 'Old Row');
        )SQL";
        REQUIRE(sqlite3_exec(raw, oldSchema, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }

    Database db(path);
    db.initSchema();

    auto channels = db.listChannels();
    REQUIRE(channels.size() == 1);
    CHECK(channels[0].id == "pre-existing");
    CHECK_FALSE(channels[0].noiseSquelchThreshold_dB.has_value());

    ChannelConfig cc = channels[0];
    cc.noiseSquelchThreshold_dB = 15.0;
    db.upsertChannel(cc);
    channels = db.listChannels();
    REQUIRE(channels[0].noiseSquelchThreshold_dB.has_value());
    CHECK(*channels[0].noiseSquelchThreshold_dB == 15.0);

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
