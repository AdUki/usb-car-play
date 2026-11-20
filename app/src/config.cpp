#include "config.h"

#include <fstream>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool Config::load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::string line, section;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(n) + ": expected key = value";
            return false;
        }
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        values_[section.empty() ? key : section + "." + key] = value;
    }
    return true;
}

std::string Config::str(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

long Config::num(const std::string& key, long def) const {
    auto it = values_.find(key);
    return it == values_.end() || it->second.empty() ? def : std::stol(it->second, nullptr, 0);
}

double Config::real(const std::string& key, double def) const {
    auto it = values_.find(key);
    return it == values_.end() || it->second.empty() ? def : std::stod(it->second);
}

bool Config::flag(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& v = it->second;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
