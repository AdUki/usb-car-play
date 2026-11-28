#include "config.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

std::string trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
	for (char& c : s) c = char(tolower(uint8_t(c)));
	return s;
}

}  // namespace

bool Config::load(const std::string& path)
{
	std::ifstream in(path);
	if (!in) return false;
	std::string line, section;
	for (int n = 1; std::getline(in, line); n++) {
		if (n == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // Notepad's BOM
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		std::string where = path + ":" + std::to_string(n);
		if (line[0] == '[') {
			size_t end = line.find(']');
			if (end == std::string::npos) {
				warnings_.push_back(where + ": no ] after the section name, line ignored");
				continue;
			}
			section = trim(line.substr(1, end - 1));
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			warnings_.push_back(where + ": not key = value, line ignored: " + line);
			continue;
		}
		std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
		if (key.empty()) {
			warnings_.push_back(where + ": no key before =, line ignored");
			continue;
		}
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);
		if (!section.empty()) key = section + "." + key;
		auto it = values_.find(key);
		if (it != values_.end() && it->second.value != value)
			changes_.push_back(key + " = " + value + " (was " + it->second.value + ")");
		values_[key] = {value, where};
	}
	return true;
}

const Config::Value* Config::find(const std::string& key) const
{
	asked_.insert(key);
	auto it = values_.find(key);
	return it == values_.end() ? nullptr : &it->second;
}

void Config::warn(const Value& v, const std::string& key, const std::string& what) const
{
	warnings_.push_back(v.where + ": " + key + " = " + v.value + ": " + what);
}

void Config::reject(const std::string& key, const std::string& why) const
{
	if (const Value* v = find(key)) warn(*v, key, why);
}

std::string Config::str(const std::string& key, const std::string& def) const
{
	const Value* v = find(key);
	return v ? v->value : def;
}

long Config::num(const std::string& key, long def) const
{
	const Value* v = find(key);
	if (!v || v->value.empty()) return def;
	char* end = nullptr;
	errno = 0;
	long n = strtol(v->value.c_str(), &end, 10);
	if (errno || end == v->value.c_str() || !trim(end).empty()) {
		warn(*v, key, "not a whole number, using " + std::to_string(def));
		return def;
	}
	return n;
}

double Config::real(const std::string& key, double def) const
{
	const Value* v = find(key);
	if (!v || v->value.empty()) return def;
	char* end = nullptr;
	errno = 0;
	double d = strtod(v->value.c_str(), &end);
	if (errno || end == v->value.c_str() || !trim(end).empty()) {
		char buf[32];
		snprintf(buf, sizeof buf, "%g", def);
		warn(*v, key, std::string("not a number, using ") + buf);
		return def;
	}
	return d;
}

bool Config::flag(const std::string& key, bool def) const
{
	const Value* v = find(key);
	if (!v) return def;
	std::string s = lower(v->value);
	if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
	if (s == "0" || s == "false" || s == "no" || s == "off") return false;
	warn(*v, key, std::string("not true or false, using ") + (def ? "true" : "false"));
	return def;
}

std::vector<std::string> Config::unknown_keys() const
{
	std::vector<std::string> out;
	for (auto& [key, v] : values_)
		if (!asked_.count(key)) out.push_back(key + " (" + v.where + ")");
	return out;
}
