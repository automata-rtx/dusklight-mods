#include "fx_preset.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace rsp {
namespace {

std::string trim(const std::string& s, const char* chars = " \t\r\n") {
    const size_t first = s.find_first_not_of(chars);
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = s.find_last_not_of(chars);
    return s.substr(first, last - first + 1);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

bool IniFile::load(const std::filesystem::path& path) {
    _sections.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    std::string section;
    std::istringstream lines(text);
    std::string raw;
    while (std::getline(lines, raw)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '/' || line[0] == '#') {
            continue;
        }
        if (line[0] == '[') {
            section = trim(line.substr(0, line.find(']')), " \t[]");
            continue;
        }
        const size_t assign = line.find('=');
        if (assign == std::string::npos) {
            _sections[section].insert({line, {}});
            continue;
        }
        const std::string key = trim(line.substr(0, assign));
        const std::string value = trim(line.substr(assign + 1));
        Values& elements = _sections[section][key];
        if (value.empty()) {
            continue;
        }
        std::string element;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == ',') {
                if (i + 1 < value.size() && value[i + 1] == ',') {
                    element += ',';
                    ++i;
                    continue;
                }
                elements.push_back(std::move(element));
                element.clear();
                continue;
            }
            element += value[i];
        }
        elements.push_back(std::move(element));
    }
    return true;
}

bool IniFile::save(const std::filesystem::path& path) const {
    std::vector<std::string> sectionNames;
    for (const auto& [name, keys] : _sections) {
        sectionNames.push_back(name);
    }
    std::sort(sectionNames.begin(), sectionNames.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
    std::string data;
    for (const std::string& sectionName : sectionNames) {
        const auto& keys = _sections.at(sectionName);
        if (keys.empty()) {
            continue;
        }
        std::vector<std::string> keyNames;
        for (const auto& [key, values] : keys) {
            keyNames.push_back(key);
        }
        std::sort(keyNames.begin(), keyNames.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
        if (!sectionName.empty()) {
            data += '[' + sectionName + "]\n";
        }
        for (const std::string& key : keyNames) {
            data += key + '=';
            std::string value;
            for (const std::string& element : keys.at(key)) {
                if (element.empty()) {
                    continue;
                }
                for (const char c : element) {
                    value.append(c == ',' ? 2 : 1, c);
                }
                value += ',';
            }
            if (!value.empty()) {
                value.pop_back();
            }
            data += value + '\n';
        }
        data += '\n';
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

bool IniFile::has(const std::string& section, const std::string& key) const { return find(section, key) != nullptr; }

const IniFile::Values* IniFile::find(const std::string& section, const std::string& key) const {
    const auto s = _sections.find(section);
    if (s == _sections.end()) {
        return nullptr;
    }
    const auto k = s->second.find(key);
    return k != s->second.end() ? &k->second : nullptr;
}

void IniFile::set(const std::string& section, const std::string& key, Values values) {
    _sections[section][key] = std::move(values);
}

void IniFile::remove(const std::string& section, const std::string& key) {
    const auto s = _sections.find(section);
    if (s != _sections.end()) {
        s->second.erase(key);
    }
}

std::vector<std::pair<std::string, std::string>> IniFile::get_definitions(const std::string& section, const std::string& key) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (const Values* values = find(section, key)) {
        for (const std::string& v : *values) {
            const size_t eq = v.find('=');
            if (eq == std::string::npos) {
                out.emplace_back(v, std::string());
            } else {
                out.emplace_back(v.substr(0, eq), v.substr(eq + 1));
            }
        }
    }
    return out;
}

void IniFile::set_definitions(const std::string& section, const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& definitions) {
    if (definitions.empty()) {
        remove(section, key);
        return;
    }
    Values values;
    for (const auto& [name, value] : definitions) {
        values.push_back(value.empty() ? name : name + '=' + value);
    }
    set(section, key, std::move(values));
}

std::string format_float(float v) { return std::to_string(v); }

std::vector<std::pair<std::string, std::string>> parse_definitions(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string item;
    const auto flush = [&] {
        const std::string t = trim(item);
        item.clear();
        if (t.empty()) {
            return;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            out.emplace_back(t, std::string());
        } else {
            out.emplace_back(trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
        }
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == '\n') {
            flush();
        } else {
            item += c;
        }
    }
    flush();
    return out;
}

std::string format_definitions(const std::vector<std::pair<std::string, std::string>>& definitions) {
    std::string out;
    for (const auto& [name, value] : definitions) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value.empty() ? name : name + '=' + value;
    }
    return out;
}

} // namespace rsp
