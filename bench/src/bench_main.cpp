#include "legato/core/json_mini.hpp"
#include "legato/legato.h"
#include "sha256.hpp"
#include "yaml_mini.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <time.h>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream output(path.c_str(), std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot write " + path);
    }
    output << contents;
}

std::int64_t parse_duration_us(const std::string& text) {
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, "ms") == 0) {
        return std::llround(std::stod(text.substr(0, text.size() - 2)) * 1000.0);
    }
    if (!text.empty() && text.back() == 's') {
        return std::llround(std::stod(text.substr(0, text.size() - 1)) * 1000000.0);
    }
    return std::stoll(text);
}

std::int64_t percentile(std::vector<std::int64_t> samples, double p) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    int rank = static_cast<int>(std::ceil(p * static_cast<double>(samples.size())));
    if (rank < 1) {
        rank = 1;
    }
    if (rank > static_cast<int>(samples.size())) {
        rank = static_cast<int>(samples.size());
    }
    return samples[static_cast<size_t>(rank - 1)];
}

enum class StepKind { Load, Play, Pause, Stop, WaitFirstFrame, Watch, SwipeNext, Seek, SeekBurst };

struct Step {
    StepKind kind = StepKind::Load;
    std::int64_t watch_us = 0;
    std::int64_t position_us = 0;
    int burst = 1;
};

struct Action {
    int repeat = 1;
    std::vector<Step> steps;
};

struct Scenario {
    std::string id;
    std::string preset = "vod";
    std::string tier = "T1";
    std::string uri;
    std::string playlist;
    int count = 0;
    std::string network_trace;
    int repetitions = 1;
    int warmup = 0;
    std::int64_t seek_tolerance_us = -1;
    int max_seek_commits = -1;
    std::vector<std::string> metrics;
    std::vector<Action> actions;
};

Step parse_step(const legato::bench::YamlNode& node) {
    Step step;
    if (node.kind == legato::bench::YamlNode::Scalar) {
        if (node.scalar == "load") {
            step.kind = StepKind::Load;
        } else if (node.scalar == "play") {
            step.kind = StepKind::Play;
        } else if (node.scalar == "pause") {
            step.kind = StepKind::Pause;
        } else if (node.scalar == "stop") {
            step.kind = StepKind::Stop;
        } else if (node.scalar == "wait_first_frame") {
            step.kind = StepKind::WaitFirstFrame;
        } else if (node.scalar == "swipe_next") {
            step.kind = StepKind::SwipeNext;
        } else {
            throw std::runtime_error("unknown action " + node.scalar);
        }
        return step;
    }
    if (node.kind != legato::bench::YamlNode::Map) {
        throw std::runtime_error("action must be a string or a map");
    }
    const legato::bench::YamlNode* watch = node.find("watch");
    if (watch != nullptr && watch->kind == legato::bench::YamlNode::Scalar) {
        step.kind = StepKind::Watch;
        step.watch_us = parse_duration_us(watch->scalar);
        return step;
    }
    const legato::bench::YamlNode* seek = node.find("seek");
    if (seek != nullptr && seek->kind == legato::bench::YamlNode::Scalar) {
        step.kind = StepKind::Seek;
        step.position_us = parse_duration_us(seek->scalar);
        return step;
    }
    const legato::bench::YamlNode* burst = node.find("seek_burst");
    if (burst != nullptr && burst->kind == legato::bench::YamlNode::Map) {
        step.kind = StepKind::SeekBurst;
        step.burst = std::stoi(burst->get("count", "1"));
        step.position_us = parse_duration_us(burst->get("to", "0s"));
        return step;
    }
    throw std::runtime_error("unsupported action map");
}

Scenario parse_scenario(const legato::bench::YamlNode& root) {
    if (root.kind != legato::bench::YamlNode::Map) {
        throw std::runtime_error("scenario root must be a map");
    }
    Scenario scenario;
    scenario.id = root.get("id");
    if (scenario.id.empty()) {
        throw std::runtime_error("scenario id is required");
    }
    scenario.preset = root.get("preset", "vod");
    scenario.tier = root.get("tier", "T1");
    if (const legato::bench::YamlNode* media = root.find("media")) {
        scenario.uri = media->get("uri");
        scenario.playlist = media->get("playlist");
        const std::string count = media->get("count");
        if (!count.empty()) {
            scenario.count = std::stoi(count);
        }
    }
    if (const legato::bench::YamlNode* network = root.find("network")) {
        scenario.network_trace = network->get("trace");
    }
    const std::string repetitions = root.get("repetitions", "1");
    const std::string warmup = root.get("warmup", "0");
    scenario.repetitions = std::stoi(repetitions);
    scenario.warmup = std::stoi(warmup);
    scenario.seek_tolerance_us = std::stoll(root.get("seek_tolerance_us", "-1"));
    scenario.max_seek_commits = std::stoi(root.get("max_seek_commits", "-1"));
    if (const legato::bench::YamlNode* metrics = root.find("metrics")) {
        if (metrics->kind != legato::bench::YamlNode::Seq) {
            throw std::runtime_error("metrics must be a list");
        }
        for (const auto& item : metrics->items) {
            if (item.kind != legato::bench::YamlNode::Scalar) {
                throw std::runtime_error("metric name must be a string");
            }
            scenario.metrics.push_back(item.scalar);
        }
    }
    const legato::bench::YamlNode* actions = root.find("actions");
    if (actions == nullptr || actions->kind != legato::bench::YamlNode::Seq) {
        throw std::runtime_error("actions must be a list");
    }
    for (const auto& item : actions->items) {
        Action action;
        if (item.kind == legato::bench::YamlNode::Map && item.find("repeat") != nullptr) {
            action.repeat = std::stoi(item.get("repeat", "1"));
            const legato::bench::YamlNode* steps = item.find("steps");
            if (steps == nullptr || steps->kind != legato::bench::YamlNode::Seq) {
                throw std::runtime_error("repeat.steps must be a list");
            }
            for (const auto& step : steps->items) {
                action.steps.push_back(parse_step(step));
            }
        } else {
            action.steps.push_back(parse_step(item));
        }
        scenario.actions.push_back(action);
    }
    return scenario;
}

struct Breakdown {
    std::int64_t probe = 0;
    std::int64_t first_packet = 0;
    std::int64_t decoder_open = 0;
    std::int64_t decoded = 0;
    std::int64_t presented = 0;
    bool cache_hit = false;
    bool decoder_reused = false;
};

struct Measured {
    std::vector<std::int64_t> first_frame_ms;
    std::vector<std::string> events;
    int presented = 0;
    int reused = 0;
    std::uint64_t alloc_steady = 0;
    std::int64_t av_sync_us = 0;
    std::vector<std::int64_t> av_sync_ms;
    std::string presentation_sha256;
    std::int64_t seek_error_us = -1;
    int seek_stale_frames = 0;
    int seek_commits = 0;
    Breakdown breakdown;
    bool have_breakdown = false;
    int runs = 0;
};

void drain(legato_player* player, std::vector<std::string>* events, bool record, int* command_done) {
    for (;;) {
        const legato_event* event = legato_player_next_event(player);
        if (event == nullptr) {
            return;
        }
        if (command_done != nullptr && event->type == LEGATO_EVENT_TYPE_COMMAND_DONE) {
            ++(*command_done);
        }
        if (record && events != nullptr) {
            events->push_back(legato_event_type_name(event->type));
        }
    }
}

void wait_commands(legato_player* player, std::vector<std::string>* events, bool record, int need) {
    int got = 0;
    for (int spin = 0; spin < 20000 && got < need; ++spin) {
        const int before = got;
        drain(player, events, record, &got);
        if (got >= need) {
            return;
        }
        if (got == before) {
            const timespec pause = {0, 2000000};
            nanosleep(&pause, nullptr);
        }
    }
    throw std::runtime_error("command did not finish");
}

bool wait_first_frame(legato_engine* engine, legato_player* player, std::vector<std::string>* events, bool record) {
    std::int64_t stepped = 0;
    while (stepped < 2000000) {
        if (legato_engine_advance_us(engine, 1000) != 0) {
            return false;
        }
        stepped += 1000;
        bool saw = false;
        for (;;) {
            const legato_event* event = legato_player_next_event(player);
            if (event == nullptr) {
                break;
            }
            if (event->type == LEGATO_EVENT_TYPE_FIRST_FRAME) {
                saw = true;
            }
            if (record && events != nullptr) {
                events->push_back(legato_event_type_name(event->type));
            }
        }
        if (saw) {
            return true;
        }
    }
    return false;
}

std::string playlist_uri(const std::string& playlist, int index) {
    std::string prefix = playlist;
    if (prefix.size() >= 2 && prefix.compare(prefix.size() - 2, 2, "/*") == 0) {
        prefix.erase(prefix.size() - 2);
    }
    return prefix + "/" + std::to_string(index);
}

void append_sync(legato_player* player, Measured* measured) {
    const int count = legato_player_copy_av_sync_us(player, nullptr, 0);
    if (count <= 0 || measured == nullptr) {
        return;
    }
    std::vector<std::int64_t> raw(static_cast<std::size_t>(count));
    legato_player_copy_av_sync_us(player, raw.data(), count);
    for (std::int64_t sample : raw) {
        const std::int64_t ms = sample / 1000;
        measured->av_sync_ms.push_back(ms);
        if (sample > measured->av_sync_us) {
            measured->av_sync_us = sample;
        }
    }
}

void post_load(legato_player* player, const std::string& uri) {
    legato_media_item item;
    std::memset(&item, 0, sizeof(item));
    item.struct_size = static_cast<uint32_t>(sizeof(item));
    item.uri = uri.c_str();
    if (legato_player_load(player, &item) != 0) {
        throw std::runtime_error("load failed for " + uri);
    }
}

struct Session {
    legato_engine* engine = nullptr;
    legato_player* player = nullptr;
    ~Session() {
        if (player != nullptr) {
            legato_player_release(player);
        }
        if (engine != nullptr) {
            legato_engine_destroy(engine);
        }
    }
};

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

void run_once(const Scenario& scenario, Measured* measured, bool record) {
    Session session;
#ifndef LEGATO_SOURCE_DIR
#define LEGATO_SOURCE_DIR "."
#endif
    const std::string config = std::string("{\"mode\":\"headless\",\"corpus_root\":\"") +
                               json_escape(LEGATO_SOURCE_DIR) + "\"}";
    session.engine = legato_engine_create(config.c_str());
    if (session.engine == nullptr) {
        throw std::runtime_error("engine_create failed");
    }
    const std::string options = std::string("{\"preset\":\"") + scenario.preset + "\"}";
    session.player = legato_player_create(session.engine, options.c_str());
    if (session.player == nullptr) {
        throw std::runtime_error("player_create failed for preset " + scenario.preset);
    }
    legato_engine* engine = session.engine;
    legato_player* player = session.player;

    int index = 0;
    std::string current = scenario.uri;
    if (!scenario.playlist.empty()) {
        current = playlist_uri(scenario.playlist, index);
        post_load(player, current);
        if (legato_player_play(player) != 0) {
            throw std::runtime_error("play failed");
        }
        wait_commands(player, &measured->events, record, 2);
    }

    bool failed = false;
    std::int64_t pending_seek_us = -1;
    for (const Action& action : scenario.actions) {
        for (int n = 0; n < action.repeat; ++n) {
            for (const Step& step : action.steps) {
                if (step.kind == StepKind::Load) {
                    if (!scenario.playlist.empty()) {
                        current = playlist_uri(scenario.playlist, index);
                    }
                    post_load(player, current);
                    wait_commands(player, &measured->events, record, 1);
                } else if (step.kind == StepKind::Play) {
                    if (legato_player_play(player) != 0) {
                        throw std::runtime_error("play failed");
                    }
                    wait_commands(player, &measured->events, record, 1);
                } else if (step.kind == StepKind::Pause) {
                    legato_player_pause(player);
                    wait_commands(player, &measured->events, record, 1);
                } else if (step.kind == StepKind::Stop) {
                    legato_player_command(player, "{\"cmd\":\"stop\"}");
                    wait_commands(player, &measured->events, record, 1);
                } else if (step.kind == StepKind::Seek) {
                    if (legato_player_seek(player, step.position_us, LEGATO_SEEK_MODE_PRECISE) != 0) {
                        throw std::runtime_error("seek failed");
                    }
                    wait_commands(player, &measured->events, record, 1);
                    pending_seek_us = step.position_us;
                } else if (step.kind == StepKind::SeekBurst) {
                    /* 连续 Seek 合成一次执行，command_done 仍按次发出。环只有 32 格，多出来的会计入 events_dropped。 */
                    legato_session_stats before;
                    std::memset(&before, 0, sizeof(before));
                    before.struct_size = static_cast<uint32_t>(sizeof(before));
                    if (legato_player_read_stats(player, &before) != 0) {
                        throw std::runtime_error("seek failed");
                    }
                    for (int i = 0; i < step.burst; ++i) {
                        if (legato_player_seek(player, step.position_us, LEGATO_SEEK_MODE_PRECISE) != 0) {
                            throw std::runtime_error("seek failed");
                        }
                    }
                    int got = 0;
                    bool finished = false;
                    for (int spin = 0; spin < 20000; ++spin) {
                        const int seen = got;
                        drain(player, &measured->events, record, &got);
                        legato_session_stats now;
                        std::memset(&now, 0, sizeof(now));
                        now.struct_size = static_cast<uint32_t>(sizeof(now));
                        if (legato_player_read_stats(player, &now) != 0) {
                            throw std::runtime_error("seek failed");
                        }
                        const int dropped = now.events_dropped - before.events_dropped;
                        if (got + (dropped > 0 ? dropped : 0) >= step.burst) {
                            finished = true;
                            break;
                        }
                        if (got == seen) {
                            const timespec pause = {0, 2000000};
                            nanosleep(&pause, nullptr);
                        }
                    }
                    if (!finished) {
                        throw std::runtime_error("command did not finish");
                    }
                    pending_seek_us = step.position_us;
                } else if (step.kind == StepKind::WaitFirstFrame) {
                    if (!wait_first_frame(engine, player, &measured->events, record)) {
                        failed = true;
                        break;
                    }
                    legato_session_stats stats;
                    std::memset(&stats, 0, sizeof(stats));
                    stats.struct_size = static_cast<uint32_t>(sizeof(stats));
                    if (legato_player_read_stats(player, &stats) != 0) {
                        failed = true;
                        break;
                    }
                    if (record) {
                        measured->first_frame_ms.push_back(stats.first_frame_us / 1000);
                        if (!measured->have_breakdown) {
                            measured->breakdown.probe = stats.stage_probe_us;
                            measured->breakdown.first_packet = stats.stage_first_packet_us;
                            measured->breakdown.decoder_open = stats.stage_decoder_open_us;
                            measured->breakdown.decoded = stats.stage_decoded_us;
                            measured->breakdown.presented = stats.stage_presented_us;
                            measured->breakdown.cache_hit = stats.cache_hit != 0;
                            measured->breakdown.decoder_reused = stats.decoder_reused_last != 0;
                            measured->have_breakdown = true;
                        }
                    }
                } else if (step.kind == StepKind::Watch) {
                    if (legato_engine_advance_us(engine, step.watch_us) != 0) {
                        failed = true;
                        break;
                    }
                    drain(player, &measured->events, record, nullptr);
                    if (record && pending_seek_us >= 0 && scenario.seek_tolerance_us >= 0) {
                        legato_session_stats seek_stats;
                        std::memset(&seek_stats, 0, sizeof(seek_stats));
                        seek_stats.struct_size = static_cast<uint32_t>(sizeof(seek_stats));
                        if (legato_player_read_stats(player, &seek_stats) != 0 || seek_stats.seek_first_pts_us < 0) {
                            throw std::runtime_error("seek did not present a frame");
                        }
                        const std::int64_t error = seek_stats.seek_first_pts_us >= pending_seek_us
                                                       ? seek_stats.seek_first_pts_us - pending_seek_us
                                                       : pending_seek_us - seek_stats.seek_first_pts_us;
                        if (error > scenario.seek_tolerance_us) {
                            throw std::runtime_error("seek first pts is outside one frame");
                        }
                        if (error > measured->seek_error_us) {
                            measured->seek_error_us = error;
                        }
                        measured->seek_stale_frames = seek_stats.seek_stale_frames;
                        measured->seek_commits = seek_stats.seek_commits;
                        pending_seek_us = -1;
                    }
                } else if (step.kind == StepKind::SwipeNext) {
                    if (scenario.count > 0 && index + 1 >= scenario.count) {
                        continue;
                    }
                    if (record) {
                        append_sync(player, measured);
                    }
                    ++index;
                    current = playlist_uri(scenario.playlist, index);
                    post_load(player, current);
                    if (legato_player_play(player) != 0) {
                        throw std::runtime_error("play failed");
                    }
                    wait_commands(player, &measured->events, record, 2);
                }
            }
            if (failed) {
                break;
            }
        }
        if (failed) {
            break;
        }
    }
    if (record && scenario.seek_tolerance_us >= 0) {
        if (measured->seek_error_us < 0) {
            throw std::runtime_error("seek accuracy was not measured");
        }
        if (measured->seek_stale_frames != 0) {
            throw std::runtime_error("old serial frame was presented");
        }
        if (scenario.max_seek_commits >= 0 && measured->seek_commits > scenario.max_seek_commits) {
            throw std::runtime_error("consecutive seeks were not coalesced");
        }
    }

    legato_session_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    stats.struct_size = static_cast<uint32_t>(sizeof(stats));
    legato_player_read_stats(player, &stats);
    if (record) {
        measured->presented += stats.presented;
        measured->reused += stats.decoder_reused;
        if (stats.alloc_steady > measured->alloc_steady) {
            measured->alloc_steady = stats.alloc_steady;
        }
        append_sync(player, measured);
        measured->presentation_sha256 = stats.presentation_sha256;
        measured->runs += 1;
    }
    if (failed) {
        throw std::runtime_error("scenario did not reach first frame: " + scenario.id);
    }
}

bool metric_known(const std::string& name) {
    return name == "first_frame_ms" || name == "stall_ratio" || name == "alloc_steady" || name == "av_sync_ms" ||
           name == "decoder_reuse_rate" || name == "first_frame_breakdown" || name == "seek_error_us" ||
           name == "seek_stale_frames" || name == "seek_commits";
}

std::string json_bool(bool value) { return value ? "true" : "false"; }

std::string json_us(std::int64_t value) {
    if (value < 0) {
        return "null";
    }
    return std::to_string(value);
}

#ifndef LEGATO_BUILD_CONFIG
#define LEGATO_BUILD_CONFIG "play-demo"
#endif
#ifndef LEGATO_FFMPEG_VERSION
#define LEGATO_FFMPEG_VERSION "5.1.6"
#endif

std::string blank_commit(std::string text) {
    const std::string key = "\"commit\": \"";
    const std::size_t pos = text.find(key);
    if (pos == std::string::npos) {
        return text;
    }
    const std::size_t start = pos + key.size();
    const std::size_t end = text.find('"', start);
    if (end == std::string::npos) {
        return text;
    }
    text.replace(start, end - start, "COMMIT");
    return text;
}

std::string format_result(const Scenario& scenario, const Measured& measured, const std::string& scenario_hash,
                          const std::string& commit) {
    const bool alloc_ok = measured.alloc_steady == 0;
    const std::int64_t av_p50 = percentile(measured.av_sync_ms, 0.50);
    const std::int64_t av_p95 = percentile(measured.av_sync_ms, 0.95);
    std::int64_t av_max = 0;
    for (std::int64_t sample : measured.av_sync_ms) {
        if (sample > av_max) {
            av_max = sample;
        }
    }
    const bool av_ok = av_p95 < 40;
    const double reuse = measured.presented == 0
                             ? 0.0
                             : static_cast<double>(measured.reused) / static_cast<double>(measured.presented);
    char reuse_buf[32];
    char stall_buf[32];
    if (!legato::json_fixed6(reuse, reuse_buf, sizeof(reuse_buf)) ||
        !legato::json_fixed6(0.0, stall_buf, sizeof(stall_buf))) {
        throw std::runtime_error("json number");
    }

    std::string out;
    out += "{\n";
    out += "  \"scenario\": {\"id\": \"" + scenario.id + "\", \"hash\": \"sha256:" + scenario_hash + "\"},\n";
    out += "  \"build\": {\"commit\": \"" + commit + "\", \"config\": \"" LEGATO_BUILD_CONFIG
           "\", \"ffmpeg\": \"" LEGATO_FFMPEG_VERSION "\", \"quirks_db\": \"none\"},\n";
    out += "  \"env\": {\"tier\": \"" + scenario.tier +
           "\", \"device\": \"headless\", \"os\": \"linux\", \"backend\": \"legato\", "
           "\"model\": \"pipeline\", \"preset\": \"" +
           scenario.preset + "\", \"simulated_network\": false, \"network_trace\": \"" + scenario.network_trace +
           "\"},\n";
    out += "  \"runs\": " + std::to_string(measured.runs) + ",\n";
    out += "  \"metrics\": {\n";
    for (size_t i = 0; i < scenario.metrics.size(); ++i) {
        const std::string& name = scenario.metrics[i];
        out += "    \"" + name + "\": ";
        if (name == "first_frame_ms") {
            out += "{\"unit\": \"ms\", \"samples\": [";
            for (size_t s = 0; s < measured.first_frame_ms.size(); ++s) {
                if (s != 0) {
                    out += ", ";
                }
                out += std::to_string(measured.first_frame_ms[s]);
            }
            const std::int64_t p50 = percentile(measured.first_frame_ms, 0.50);
            const std::int64_t p90 = percentile(measured.first_frame_ms, 0.90);
            out += "], \"p50\": " + std::to_string(p50) + ", \"p90\": " + std::to_string(p90) + "}";
        } else if (name == "stall_ratio") {
            out += std::string("{\"unit\": \"ratio\", \"value\": ") + stall_buf + "}";
        } else if (name == "alloc_steady") {
            out += "{\"unit\": \"count\", \"value\": " + std::to_string(measured.alloc_steady) + "}";
        } else if (name == "av_sync_ms") {
            out += "{\"unit\": \"ms\", \"p50\": " + std::to_string(av_p50) + ", \"p95\": " + std::to_string(av_p95) +
                   ", \"max\": " + std::to_string(av_max) + "}";
        } else if (name == "seek_error_us") {
            out += "{\"unit\": \"us\", \"value\": " + std::to_string(measured.seek_error_us) + "}";
        } else if (name == "seek_stale_frames") {
            out += "{\"unit\": \"count\", \"value\": " + std::to_string(measured.seek_stale_frames) + "}";
        } else if (name == "seek_commits") {
            out += "{\"unit\": \"count\", \"value\": " + std::to_string(measured.seek_commits) + "}";
        } else if (name == "decoder_reuse_rate") {
            out += std::string("{\"unit\": \"ratio\", \"value\": ") + reuse_buf + "}";
        } else if (name == "first_frame_breakdown") {
            out += "{\"unit\": \"us\", \"probe\": " + json_us(measured.breakdown.probe) +
                   ", \"first_packet\": " + json_us(measured.breakdown.first_packet) +
                   ", \"decoder_open\": " + json_us(measured.breakdown.decoder_open) +
                   ", \"first_frame_decoded\": " + json_us(measured.breakdown.decoded) +
                   ", \"first_frame_presented\": " + json_us(measured.breakdown.presented) +
                   ", \"cache_hit\": " + json_bool(measured.breakdown.cache_hit) +
                   ", \"decoder_reused\": " + json_bool(measured.breakdown.decoder_reused) + "}";
        }
        out += (i + 1 == scenario.metrics.size()) ? "\n" : ",\n";
    }
    out += "  },\n";
    out += "  \"events\": [";
    for (size_t i = 0; i < measured.events.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += "\"" + measured.events[i] + "\"";
    }
    out += "],\n";
    out += "  \"presentation_sha256\": \"" + measured.presentation_sha256 + "\",\n";
    out += std::string("  \"gates\": {\"alloc_steady_eq_0\": ") + json_bool(alloc_ok) +
           ", \"av_sync_p95_lt_40ms\": " + json_bool(av_ok) + "}\n";
    out += "}\n";
    return out;
}

void show_mismatch(const std::string& actual, const std::string& expected) {
    size_t index = 0;
    const size_t limit = actual.size() < expected.size() ? actual.size() : expected.size();
    while (index < limit && actual[index] == expected[index]) {
        ++index;
    }
    std::fprintf(stderr, "golden mismatch at byte %zu (actual %zu bytes, expected %zu bytes)\n", index, actual.size(),
                 expected.size());
}

}  /* namespace */

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "run") != 0) {
        std::fprintf(stderr, "usage: legato-bench run <scenario.yaml> [--golden file] [--write-golden file] [--out file]\n");
        return 2;
    }
    std::string scenario_path = argv[2];
    std::string golden_path;
    std::string write_golden_path;
    std::string out_path;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--golden") == 0 && i + 1 < argc) {
            golden_path = argv[++i];
        } else if (std::strcmp(argv[i], "--write-golden") == 0 && i + 1 < argc) {
            write_golden_path = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    try {
        const std::string text = read_file(scenario_path);
        const Scenario scenario = parse_scenario(legato::bench::yaml_parse(text));
        for (const auto& metric : scenario.metrics) {
            if (!metric_known(metric)) {
                std::fprintf(stderr, "metric not available in M0: %s\n", metric.c_str());
                return 2;
            }
        }
        Measured measured;
        for (int i = 0; i < scenario.warmup; ++i) {
            run_once(scenario, &measured, false);
        }
        for (int i = 0; i < scenario.repetitions; ++i) {
            run_once(scenario, &measured, true);
        }
        const std::string hash = legato::bench::sha256_hex(text);
#ifdef LEGATO_BUILD_COMMIT
        const std::string commit = LEGATO_BUILD_COMMIT;
#else
        const std::string commit = "unknown";
#endif
        const std::string real = format_result(scenario, measured, hash, commit);
        if (!out_path.empty()) {
            write_file(out_path, real);
        }
        if (!write_golden_path.empty()) {
            write_file(write_golden_path, real);
        }
        std::fputs(real.c_str(), stdout);
        const bool alloc_ok = measured.alloc_steady == 0;
        const bool av_ok = percentile(measured.av_sync_ms, 0.95) < 40;
        if (!alloc_ok || !av_ok) {
            std::fprintf(stderr, "T1 gate failed: alloc_steady=%llu av_sync_p95_ms=%lld\n",
                         static_cast<unsigned long long>(measured.alloc_steady),
                         static_cast<long long>(percentile(measured.av_sync_ms, 0.95)));
            return 1;
        }
        if (!golden_path.empty()) {
            const std::string expected = read_file(golden_path);
            if (blank_commit(expected) != blank_commit(real)) {
                show_mismatch(blank_commit(real), blank_commit(expected));
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "legato-bench: %s\n", error.what());
        return 2;
    }
}
