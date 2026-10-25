#pragma once

// Switches LC_NUMERIC to a locale with a decimal comma for one scope, the way
// a Qt app runs on a Portuguese, German or Swedish desktop, so a test can
// check that file parsing doesn't depend on it. Header-only, no dependencies.

#include <clocale>
#include <cstring>
#include <string>

namespace testlocale {

class CommaLocale {
public:
    CommaLocale()
    {
        if (const char* cur = std::setlocale(LC_NUMERIC, nullptr))
            previous_ = cur;
        for (const char* name : {"pt_BR.UTF-8", "de_DE.UTF-8", "sv_SE.UTF-8", "fr_FR.UTF-8"}) {
            if (std::setlocale(LC_NUMERIC, name) &&
                std::strcmp(std::localeconv()->decimal_point, ",") == 0) {
                name_ = name;
                return;
            }
        }
        std::setlocale(LC_NUMERIC, previous_.c_str());
    }
    ~CommaLocale() { std::setlocale(LC_NUMERIC, previous_.c_str()); }
    CommaLocale(const CommaLocale&) = delete;
    CommaLocale& operator=(const CommaLocale&) = delete;

    // Empty when the system has none of the locales installed.
    const std::string& name() const { return name_; }

private:
    std::string previous_ = "C";
    std::string name_;
};

} // namespace testlocale
