#include "legato/engine/corpus.hpp"

#include "sha256.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace legato {
namespace {

struct Item {
    std::string uri;
    std::string path;
    std::string path_prefix;
    std::string sha256;
    std::string sha256_list;
    int count = 1;
};

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return text.size() >= n && text.compare(0, n, prefix) == 0;
}

std::vector<Item> load_items(const std::string& file, std::string* error) {
    std::ifstream input(file.c_str());
    if (!input) {
        *error = "打不开 corpus.yaml";
        return {};
    }
    std::vector<Item> items;
    Item current;
    bool in_item = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (starts_with(line, "- id:")) {
            if (in_item) {
                items.push_back(current);
            }
            current = Item();
            in_item = true;
            continue;
        }
        if (!in_item) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "uri") {
            current.uri = value;
        } else if (key == "path") {
            current.path = value;
        } else if (key == "path_prefix") {
            current.path_prefix = value;
        } else if (key == "sha256") {
            current.sha256 = value;
        } else if (key == "sha256_list") {
            current.sha256_list = value;
        } else if (key == "count") {
            current.count = std::atoi(value.c_str());
        }
    }
    if (in_item) {
        items.push_back(current);
    }
    return items;
}

std::string file_sha256(const std::string& path, std::string* error) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        *error = "打不开这个文件";
        return {};
    }
    bench::Sha256 hasher;
    char buffer[1 << 16];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize got = input.gcount();
        if (got > 0) {
            hasher.update(buffer, static_cast<std::size_t>(got));
        }
    }
    return hasher.hex();
}

std::string list_at(const std::string& list, int index) {
    int cursor = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == ',') {
            if (cursor == index) {
                return trim(list.substr(start, i - start));
            }
            ++cursor;
            start = i + 1;
        }
    }
    return {};
}

}  /* namespace */

int resolve_corpus(const char* source_dir, const char* uri, char* path_out, int path_len, const char** err_name,
                   const char** err_message) {
    if (err_name != nullptr) {
        *err_name = "source_open_failed";
    }
    if (err_message != nullptr) {
        *err_message = "corpus 索引不可用";
    }
    if (source_dir == nullptr || uri == nullptr || path_out == nullptr || path_len < 2) {
        return -1;
    }
    const std::string catalog = std::string(source_dir) + "/testdata/corpus.yaml";
    std::string error;
    const std::vector<Item> items = load_items(catalog, &error);
    if (!error.empty()) {
        if (err_message != nullptr) {
            *err_message = "打不开 corpus.yaml";
        }
        return -1;
    }
    const std::string request(uri);
    for (const Item& item : items) {
        std::string relative;
        std::string expect;
        if (!item.uri.empty() && item.uri == request && !item.path.empty()) {
            relative = item.path;
            expect = item.sha256;
        } else if (!item.uri.empty() && item.uri.size() > 2 && item.uri.compare(item.uri.size() - 2, 2, "/*") == 0) {
            const std::string prefix = item.uri.substr(0, item.uri.size() - 1);
            if (request.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string index_text = request.substr(prefix.size());
            if (index_text.empty()) {
                continue;
            }
            char* end = nullptr;
            const long index = std::strtol(index_text.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || index < 0 || index >= item.count) {
                if (err_message != nullptr) {
                    *err_message = "corpus 序号超出范围";
                }
                return -1;
            }
            relative = item.path_prefix + std::to_string(index) + ".mp4";
            expect = list_at(item.sha256_list, static_cast<int>(index));
        } else {
            continue;
        }
        const std::string full = std::string(source_dir) + "/" + relative;
        if (std::snprintf(path_out, static_cast<std::size_t>(path_len), "%s", full.c_str()) >= path_len) {
            if (err_name != nullptr) {
                *err_name = "invalid_argument";
            }
            if (err_message != nullptr) {
                *err_message = "路径太长";
            }
            return -1;
        }
        if (expect.empty()) {
            if (err_message != nullptr) {
                *err_message = "corpus 条目没有内容哈希";
            }
            return -1;
        }
        std::string hash_error;
        const std::string actual = file_sha256(full, &hash_error);
        if (!hash_error.empty() || actual != expect) {
            if (err_message != nullptr) {
                *err_message = hash_error.empty() ? "corpus 内容哈希不一致" : "打不开这个文件";
            }
            return -1;
        }
        return 0;
    }
    if (err_name != nullptr) {
        *err_name = "unsupported_scheme";
    }
    if (err_message != nullptr) {
        *err_message = "这个协议还不能播";
    }
    return -1;
}

}  /* namespace legato */
