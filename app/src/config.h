// /etc/btdisk.conf: ini style, "[section]" then "key = value"; # starts a comment.
#pragma once

#include <map>
#include <string>

class Config {
public:
    bool load(const std::string& path, std::string& error);
    void set(const std::string& key, const std::string& value) { values_[key] = value; }

    std::string str(const std::string& key, const std::string& def) const;
    long num(const std::string& key, long def) const;
    double real(const std::string& key, double def) const;
    bool flag(const std::string& key, bool def) const;

private:
    std::map<std::string, std::string> values_;   // "section.key" -> value
};
