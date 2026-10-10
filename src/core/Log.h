#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// Opt-in progress log: with ORBIS_VERBOSE=1 set, informational lines (which
// decode/stitch path a clip got, sync / seam measurements starting and their
// results) go to stderr. Warnings and fallbacks print regardless.
namespace core::log {

inline bool Verbose()
{
    static const bool on = [] {
        const char *v = std::getenv("ORBIS_VERBOSE");
        return v && *v && *v != '0';
    }();
    return on;
}

// printf-style; prints only when Verbose().
[[gnu::format(printf, 1, 2)]] inline void Info(const char *fmt, ...)
{
    if (!Verbose())
        return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

} // namespace core::log
