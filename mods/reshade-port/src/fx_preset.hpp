// ReShade's INI dialect, for ReShadePreset.ini files.
//
// Same rules as ReShade's ini_file: values are comma-separated lists (",," escapes a comma), the
// unnamed root section comes first, and sections and keys are written sorted case-insensitively so
// a preset round-trips with ReShade's own files. Lines starting with ';', '/' or '#' are comments.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rsp {

class IniFile {
public:
    using Values = std::vector<std::string>;

    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    bool has(const std::string& section, const std::string& key) const;
    const Values* find(const std::string& section, const std::string& key) const;
    void set(const std::string& section, const std::string& key, Values values);
    void remove(const std::string& section, const std::string& key);
    void clear() { _sections.clear(); }

    // NAME=VALUE list values (PreprocessorDefinitions).
    std::vector<std::pair<std::string, std::string>> get_definitions(const std::string& section, const std::string& key) const;
    void set_definitions(const std::string& section, const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& definitions);

    const std::map<std::string, std::map<std::string, Values>>& sections() const { return _sections; }

private:
    std::map<std::string, std::map<std::string, Values>> _sections;
};

// Formats a float as ReShade does (std::to_string, six decimals).
std::string format_float(float v);

// "A=1,B" style text <-> definitions (the UI edits them as one line).
std::vector<std::pair<std::string, std::string>> parse_definitions(const std::string& text);
std::string format_definitions(const std::vector<std::pair<std::string, std::string>>& definitions);

} // namespace rsp
