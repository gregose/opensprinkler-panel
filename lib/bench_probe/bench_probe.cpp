#include "bench_probe.h"

#include <cctype>
#include <cstddef>

namespace bench {
namespace {

// Advance past ASCII spaces/tabs. Returns pointer to first non-space char.
const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Case-insensitive compare of the verb token [p, end) against a literal.
bool verb_is(const char* p, const char* end, const char* literal) {
    for (; p < end && *literal; ++p, ++literal) {
        if (std::tolower(static_cast<unsigned char>(*p)) !=
            std::tolower(static_cast<unsigned char>(*literal))) {
            return false;
        }
    }
    return p == end && *literal == '\0';
}

// Parse a non-negative decimal integer starting at *pp, advancing *pp past it.
// Returns false if there is no digit or the value overflows int16 range.
bool parse_coord(const char** pp, int16_t* out) {
    const char* p = skip_ws(*pp);
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    long v = 0;
    while (std::isdigit(static_cast<unsigned char>(*p))) {
        v = v * 10 + (*p - '0');
        if (v > 32767) return false;  // out of int16 range
        ++p;
    }
    *out = static_cast<int16_t>(v);
    *pp = p;
    return true;
}

// A two-coordinate command (TAP/DOWN/MOVE): parse "x y", require nothing but
// trailing whitespace afterwards.
Command two_arg(Cmd cmd, const char* args) {
    Command c;
    int16_t x = 0, y = 0;
    if (!parse_coord(&args, &x)) return {Cmd::Invalid, 0, 0};
    if (!parse_coord(&args, &y)) return {Cmd::Invalid, 0, 0};
    if (*skip_ws(args) != '\0') return {Cmd::Invalid, 0, 0};  // trailing junk
    c.cmd = cmd;
    c.x = x;
    c.y = y;
    return c;
}

}  // namespace

Command parse_command(const char* line) {
    if (line == nullptr) return {Cmd::None, 0, 0};

    const char* p = skip_ws(line);
    if (*p == '\0') return {Cmd::None, 0, 0};  // blank line

    // Isolate the verb token.
    const char* verb = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') ++p;
    const char* verb_end = p;
    const char* args = p;  // remainder (may be all whitespace / empty)

    if (verb_is(verb, verb_end, "SHOT")) {
        if (*skip_ws(args) != '\0') return {Cmd::Invalid, 0, 0};
        return {Cmd::Shot, 0, 0};
    }
    if (verb_is(verb, verb_end, "UP")) {
        if (*skip_ws(args) != '\0') return {Cmd::Invalid, 0, 0};
        return {Cmd::Up, 0, 0};
    }
    if (verb_is(verb, verb_end, "TAP"))  return two_arg(Cmd::Tap, args);
    if (verb_is(verb, verb_end, "DOWN")) return two_arg(Cmd::Down, args);
    if (verb_is(verb, verb_end, "MOVE")) return two_arg(Cmd::Move, args);

    return {Cmd::Invalid, 0, 0};
}

}  // namespace bench
