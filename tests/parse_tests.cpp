#include "sha256.hpp"
#include "yaml_mini.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL parse_tests:%d %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

std::string read_file(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void test_sha() {
    CHECK(legato::bench::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(legato::bench::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_yaml_string() {
    const char* text =
        "id: startup-local\n"
        "media:\n"
        "  uri: corpus://startup/local-1080p-h264\n"
        "actions:\n"
        "  - load\n"
        "  - watch: 5s\n"
        "  - repeat: 2\n"
        "    steps:\n"
        "      - wait_first_frame\n"
        "      - swipe_next\n";
    const legato::bench::YamlNode root = legato::bench::yaml_parse(text);
    CHECK(root.get("id") == "startup-local");
    const legato::bench::YamlNode* media = root.find("media");
    CHECK(media != nullptr && media->get("uri") == "corpus://startup/local-1080p-h264");
    const legato::bench::YamlNode* actions = root.find("actions");
    CHECK(actions != nullptr && actions->kind == legato::bench::YamlNode::Seq);
    CHECK(actions->items.size() == 3);
    CHECK(actions->items[0].scalar == "load");
    CHECK(actions->items[1].get("watch") == "5s");
    CHECK(actions->items[2].get("repeat") == "2");
    const legato::bench::YamlNode* steps = actions->items[2].find("steps");
    CHECK(steps != nullptr && steps->items.size() == 2);
    CHECK(steps->items[1].scalar == "swipe_next");
}

void test_scenario_files() {
    const std::string startup = read_file(std::string(LEGATO_SOURCE_DIR) + "/bench/scenarios/startup-local.yaml");
    const legato::bench::YamlNode root = legato::bench::yaml_parse(startup);
    CHECK(root.get("id") == "startup-local");
    CHECK(root.get("preset") == "vod");
    const std::string feed = read_file(std::string(LEGATO_SOURCE_DIR) + "/bench/scenarios/feed-swipe-4g.yaml");
    const legato::bench::YamlNode feed_root = legato::bench::yaml_parse(feed);
    CHECK(feed_root.get("preset") == "short_video");
    const legato::bench::YamlNode* actions = feed_root.find("actions");
    CHECK(actions != nullptr && actions->items.size() == 1);
    CHECK(actions->items[0].get("repeat") == "30");
    const legato::bench::YamlNode* steps = actions->items[0].find("steps");
    CHECK(steps != nullptr && steps->items.size() == 3);
    CHECK(steps->items[1].get("watch") == "3s");
}

}  /* namespace */

int main() {
    test_sha();
    test_yaml_string();
    test_scenario_files();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
