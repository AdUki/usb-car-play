/// @file
/// The settings files: ini style, "[section]" then "key = value"; # or ; starts a comment.
///
/// Several files load on top of each other (/etc/btdisk.conf, then /boot/btdisk.txt, the one a PC
/// edits): a key set again wins. People edit these by hand, so nothing in them can stop btdisk: a
/// line it can't read, a value that isn't a number, a key nobody asks for, each is a warning and
/// the setting keeps its default.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

/// The settings, as "section.key" -> value, from every file loaded.
class Config {
public:
    /// Adds the file's settings.
    /// @return false only when it can't be read
    bool load(const std::string& path);
    void set(const std::string& key, const std::string& value) { values_[key] = {value, "set"}; }

    std::string str(const std::string& key, const std::string& def) const;
    long num(const std::string& key, long def) const;
    double real(const std::string& key, double def) const;
    bool flag(const std::string& key, bool def) const;
    /// A value that was read but makes no sense ("tags = both"): a warning naming its line.
    void reject(const std::string& key, const std::string& why) const;

    /// What to log once the log is set up (the files load before it is).
    const std::vector<std::string>& warnings() const { return warnings_; }
    /// The keys a later file set to something else than an earlier one ("audio.format = wav
    /// (was mp3)"), in the order they were met.
    const std::vector<std::string>& changes() const { return changes_; }
    /// Keys set in a file that nothing asked for: a typo, or a setting that no longer exists.
    std::vector<std::string> unknown_keys() const;

private:
    struct Value {
        std::string value;
        /// file:line
        std::string where;
    };
    const Value* find(const std::string& key) const;
    void warn(const Value& v, const std::string& key, const std::string& what) const;

    /// "section.key" -> value
    std::map<std::string, Value> values_;
    mutable std::set<std::string> asked_;
    mutable std::vector<std::string> warnings_;
    std::vector<std::string> changes_;
};
