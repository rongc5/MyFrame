#ifndef MYFRAME_INI_CONFIG_H
#define MYFRAME_INI_CONFIG_H

/*
 * Header-only .ini file reader for MyFrame
 * -------------------------------------------------------------
 * Usage:
 *   IniConfig cfg;
 *   if (cfg.load("config.ini")) {
 *       std::string host = cfg.value("server", "host", "127.0.0.1");
 *       int port         = cfg.intValue("server", "port", 8080);
 *   }
 *
 *  Features:
 *   • Support for [section] and key=value pairs
 *   • `#` or `;` start-of-line comments are ignored
 *   • Leading/trailing whitespace is trimmed automatically
 *   • Helper getters for string/int/double/bool
 */

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <algorithm>

namespace myframe {

class IniConfig {
public:
    bool load(const std::string &path) {
        std::ifstream in(path);
        if (!in) return false;
        std::string line, currentSection;
        while (std::getline(in, line)) {
            trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                trim(currentSection);
            } else {
                auto eqPos = line.find('=');
                if (eqPos == std::string::npos) continue; // malformed
                std::string key   = line.substr(0, eqPos);
                std::string value = line.substr(eqPos + 1);
                trim(key);
                trim(value);
                data_[currentSection][key] = value;
            }
        }
        return true;
    }

    std::string value(const std::string &section, const std::string &key,
                      const std::string &def = "") const {
        auto sit = data_.find(section);
        if (sit == data_.end()) return def;
        auto kit = sit->second.find(key);
        return kit == sit->second.end() ? def : kit->second;
    }

    int intValue(const std::string &section, const std::string &key, int def = 0) const {
        auto v = value(section, key);
        return v.empty() ? def : std::atoi(v.c_str());
    }

    double doubleValue(const std::string &section, const std::string &key, double def = 0.0) const {
        auto v = value(section, key);
        return v.empty() ? def : std::atof(v.c_str());
    }

    bool boolValue(const std::string &section, const std::string &key, bool def = false) const {
        auto v = value(section, key);
        if (v.empty()) return def;
        std::string low = v;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        return (low == "1" || low == "true" || low == "yes" || low == "on");
    }

private:
    static void ltrim(std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
            return !std::isspace(ch);
        }));
    }
    static void rtrim(std::string &s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), s.end());
    }
    static void trim(std::string &s) {
        ltrim(s);
        rtrim(s);
    }

    std::map<std::string, std::map<std::string, std::string>> data_;
};

} // namespace myframe

#endif // MYFRAME_INI_CONFIG_H
