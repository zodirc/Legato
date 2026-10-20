#ifndef LEGATO_BENCH_YAML_MINI_HPP
#define LEGATO_BENCH_YAML_MINI_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace legato {
namespace bench {

struct YamlNode {
    enum Kind { Scalar, Map, Seq } kind = Scalar;
    std::string scalar;
    std::vector<std::pair<std::string, YamlNode>> entries;
    std::vector<YamlNode> items;

    const YamlNode* find(const std::string& key) const {
        if (kind != Map) {
            return nullptr;
        }
        for (const auto& entry : entries) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    std::string get(const std::string& key, const std::string& fallback = "") const {
        const YamlNode* node = find(key);
        if (node == nullptr || node->kind != Scalar) {
            return fallback;
        }
        return node->scalar;
    }
};

class YamlError : public std::runtime_error {
public:
    explicit YamlError(const std::string& message) : std::runtime_error(message) {}
};

inline std::string yaml_strip_comment(const std::string& line) {
    bool in_string = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_string) {
            if (ch == quote) {
                in_string = false;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote = ch;
            continue;
        }
        if (ch == '#' && (i == 0 || line[i - 1] == ' ')) {
            return line.substr(0, i);
        }
    }
    return line;
}

inline bool yaml_split_key(const std::string& text, std::string* key, std::string* value) {
    bool in_string = false;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (ch == quote) {
                in_string = false;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote = ch;
            continue;
        }
        if (ch == ':') {
            *key = text.substr(0, i);
            while (!key->empty() && key->back() == ' ') {
                key->pop_back();
            }
            size_t start = i + 1;
            while (start < text.size() && text[start] == ' ') {
                ++start;
            }
            *value = text.substr(start);
            while (!value->empty() && value->back() == ' ') {
                value->pop_back();
            }
            return !key->empty();
        }
    }
    return false;
}

inline YamlNode yaml_parse(const std::string& input) {
    struct Line {
        int indent = 0;
        std::string text;
        int number = 0;
    };
    std::vector<Line> lines;
    int number = 0;
    size_t cursor = 0;
    while (cursor <= input.size()) {
        size_t end = input.find('\n', cursor);
        if (end == std::string::npos) {
            end = input.size();
        }
        std::string raw = input.substr(cursor, end - cursor);
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        ++number;
        if (raw.find('\t') != std::string::npos) {
            throw YamlError("tabs are not allowed at line " + std::to_string(number));
        }
        std::string stripped = yaml_strip_comment(raw);
        size_t last = stripped.find_last_not_of(' ');
        if (last == std::string::npos) {
            cursor = end + 1;
            if (end == input.size()) {
                break;
            }
            continue;
        }
        stripped.erase(last + 1);
        size_t first = stripped.find_first_not_of(' ');
        Line line;
        line.indent = static_cast<int>(first);
        line.text = stripped.substr(first);
        line.number = number;
        lines.push_back(line);
        if (end == input.size()) {
            break;
        }
        cursor = end + 1;
    }

    size_t index = 0;
    const auto parse_block = [&](auto& self, int min_indent) -> YamlNode {
        if (index >= lines.size() || lines[index].indent < min_indent) {
            throw YamlError("expected a nested block");
        }
        const int indent = lines[index].indent;
        if (lines[index].text.rfind("- ", 0) == 0 || lines[index].text == "-") {
            YamlNode seq;
            seq.kind = YamlNode::Seq;
            while (index < lines.size() && lines[index].indent == indent &&
                   (lines[index].text.rfind("- ", 0) == 0 || lines[index].text == "-")) {
                const std::string payload =
                    lines[index].text == "-" ? "" : lines[index].text.substr(2);
                ++index;
                if (payload.empty()) {
                    seq.items.push_back(self(self, indent + 1));
                    continue;
                }
                std::string key;
                std::string value;
                if (!yaml_split_key(payload, &key, &value)) {
                    YamlNode scalar;
                    scalar.kind = YamlNode::Scalar;
                    scalar.scalar = payload;
                    seq.items.push_back(scalar);
                    continue;
                }
                YamlNode map;
                map.kind = YamlNode::Map;
                auto add_value = [&](const std::string& item_key, const std::string& item_value) {
                    if (item_value.empty()) {
                        map.entries.emplace_back(item_key, self(self, indent + 1));
                    } else {
                        YamlNode scalar;
                        scalar.kind = YamlNode::Scalar;
                        scalar.scalar = item_value;
                        map.entries.emplace_back(item_key, scalar);
                    }
                };
                add_value(key, value);
                while (index < lines.size() && lines[index].indent > indent) {
                    if (lines[index].text.rfind("- ", 0) == 0) {
                        throw YamlError("unexpected list item at line " + std::to_string(lines[index].number));
                    }
                    std::string next_key;
                    std::string next_value;
                    if (!yaml_split_key(lines[index].text, &next_key, &next_value)) {
                        throw YamlError("expected key at line " + std::to_string(lines[index].number));
                    }
                    ++index;
                    add_value(next_key, next_value);
                }
                seq.items.push_back(map);
            }
            return seq;
        }

        YamlNode map;
        map.kind = YamlNode::Map;
        while (index < lines.size() && lines[index].indent == indent) {
            std::string key;
            std::string value;
            if (!yaml_split_key(lines[index].text, &key, &value)) {
                throw YamlError("expected key at line " + std::to_string(lines[index].number));
            }
            ++index;
            if (value.empty()) {
                map.entries.emplace_back(key, self(self, indent + 1));
            } else {
                YamlNode scalar;
                scalar.kind = YamlNode::Scalar;
                scalar.scalar = value;
                map.entries.emplace_back(key, scalar);
            }
        }
        return map;
    };
    if (lines.empty()) {
        return YamlNode{};
    }
    return parse_block(parse_block, 0);
}

}  /* namespace bench */
}  /* namespace legato */

#endif
