#include "SecretFile.h"

#include <fstream>

namespace mangos::patcher {

bool ReadSecretField(const std::string& path, const std::string& field,
                     std::string& out, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            const std::string space = " \t\r\n\"";
            const std::size_t first = s.find_first_not_of(space);
            if (first == std::string::npos) {
                s.clear();
                return;
            }
            s = s.substr(first, s.find_last_not_of(space) - first + 1);
        };
        trim(key);
        trim(value);

        if (key == field) {
            out = value;
            return true;
        }
    }

    error = path + ": no field '" + field + "'";
    return false;
}

}  // namespace mangos::patcher
