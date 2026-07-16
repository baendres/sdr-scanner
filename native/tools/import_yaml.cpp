// One-shot migration helper: reads an existing Python sdr-scanner YAML config
// (see ../../example-sdrscan.yaml) and seeds a fresh SQLite database with it.
//
// Usage: sdrscan_import_yaml <sdrscan.yaml> <sdrscan.db>
//
// This is intentionally a separate small binary rather than something the main
// app does automatically - config in the DB is meant to be the durable source
// of truth going forward; importing is a one-time, explicit action.

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <sstream>

#include "../src/db/Database.h"
#include "../src/util/Uuid.h"

using sdrscan::makeUuid;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <sdrscan.yaml> <sdrscan.db>\n";
        return 1;
    }

    YAML::Node config = YAML::LoadFile(argv[1]);
    sdrscan::Database db(argv[2]);
    db.initSchema();

    if (!db.isEmpty()) {
        std::cerr << "Warning: database already has channels/receivers configured; "
                     "imported entries will be added alongside them.\n";
    }

    ///
    // Scanner settings
    if (config["scanner"] && config["scanner"]["maxChannelsPerWindow"]) {
        db.saveScannerSetting("maxChannelsPerWindow",
                               std::to_string(config["scanner"]["maxChannelsPerWindow"].as<int>()));
    }

    ///
    // Receivers
    int rxOrder = 0;
    if (config["receivers"]) {
        for (const auto& rx : config["receivers"]) {
            sdrscan::ReceiverConfig rc;
            rc.id = makeUuid();
            std::string typeStr = rx["type"].as<std::string>();
            auto type = sdrscan::receiverTypeFromString(typeStr);
            if (!type) {
                std::cerr << "Skipping receiver with unknown type: " << typeStr << "\n";
                continue;
            }
            rc.type = *type;
            if (rx["deviceArg"]) rc.deviceArg = rx["deviceArg"].as<std::string>();
            if (rx["driver"]) rc.driver = rx["driver"].as<std::string>();
            if (rx["gain"]) rc.gain = rx["gain"].as<double>();
            if (rx["gains"]) {
                for (const auto& g : rx["gains"]) {
                    rc.gains[g.first.as<std::string>()] = g.second.as<double>();
                }
            }
            rc.sortOrder = rxOrder++;
            db.upsertReceiver(rc);
            std::cout << "Imported receiver: " << typeStr << "\n";
        }
    }

    ///
    // Channel defaults
    sdrscan::ChannelConfig defaults;
    if (config["channel_defaults"]) {
        const auto& d = config["channel_defaults"];
        if (d["mode"]) defaults.mode = sdrscan::channelModeFromString(d["mode"].as<std::string>()).value_or(sdrscan::ChannelMode::FM);
        if (d["audioGain_dB"]) defaults.audioGain_dB = d["audioGain_dB"].as<double>();
        if (d["squelchThreshold"]) defaults.squelchThreshold = d["squelchThreshold"].as<double>();
        if (d["dwellTime_s"]) defaults.dwellTime_s = d["dwellTime_s"].as<double>();
    }

    ///
    // Channels
    int chOrder = 0;
    if (config["channels"]) {
        for (const auto& ch : config["channels"]) {
            sdrscan::ChannelConfig cc = defaults;
            cc.id = makeUuid();
            cc.freq_hz = static_cast<int64_t>(ch["freq"].as<double>() * 1e6);
            cc.label = ch["label"] ? ch["label"].as<std::string>() : std::to_string(ch["freq"].as<double>());
            if (ch["mode"]) {
                auto mode = sdrscan::channelModeFromString(ch["mode"].as<std::string>());
                if (!mode) {
                    std::cerr << "Skipping channel with unknown mode: " << ch["mode"].as<std::string>() << "\n";
                    continue;
                }
                cc.mode = *mode;
            }
            if (ch["audioGain_dB"]) cc.audioGain_dB = ch["audioGain_dB"].as<double>();
            if (ch["squelchThreshold"]) cc.squelchThreshold = ch["squelchThreshold"].as<double>();
            if (ch["dwellTime_s"]) cc.dwellTime_s = ch["dwellTime_s"].as<double>();
            cc.sortOrder = chOrder++;
            db.upsertChannel(cc);
            std::cout << "Imported channel: " << cc.label << " (" << (cc.freq_hz / 1e6) << " MHz)\n";
        }
    }

    ///
    // Outputs
    if (config["outputs"]) {
        for (const auto& out : config["outputs"]) {
            sdrscan::OutputConfig oc;
            oc.type = out["type"].as<std::string>();
            YAML::Emitter emitter;
            // Store the remaining fields as JSON via a simple manual pass (avoids pulling
            // yaml-cpp -> json conversion machinery for a handful of known keys).
            std::ostringstream json;
            json << "{";
            bool first = true;
            for (const auto& kv : out) {
                std::string key = kv.first.as<std::string>();
                if (key == "type") continue;
                if (!first) json << ",";
                first = false;
                json << "\"" << key << "\":\"" << kv.second.as<std::string>() << "\"";
            }
            json << "}";
            oc.configJson = json.str();
            db.upsertOutput(oc);
            std::cout << "Imported output: " << oc.type << "\n";
        }
    }

    std::cout << "Import complete: " << argv[2] << "\n";
    return 0;
}
