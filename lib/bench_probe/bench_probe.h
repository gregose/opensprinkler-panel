// bench_probe — pure parser for the on-device bench probe protocol.
//
// The firmware exposes a tiny line-based command channel over the existing
// dev-log TCP socket (port 2323, gated behind the `dev_log` NVS flag). A bench
// host sends one ASCII command per line to drive the panel without physical
// touch and to pull a pixel-exact screenshot of whatever LVGL last drew:
//
//   SHOT              -> capture the current screen and stream it back
//   TAP <x> <y>       -> synthesize a press+release (a click) at (x,y)
//   DOWN <x> <y>      -> synthesize touch-down (press held)
//   MOVE <x> <y>      -> synthesize a move while pressed
//   UP                -> synthesize touch-up (release)
//
// This translation unit is intentionally free of Arduino/LVGL/network deps so
// it compiles and is unit-tested natively (env:native), per the project's
// "hardware-independent logic lives in lib/*" rule. The firmware glue in
// src/main.cpp parses each inbound line with parse_command() and dispatches.

#pragma once

#include <cstdint>

namespace bench {

enum class Cmd : uint8_t {
    None,     // blank line — ignore
    Shot,     // SHOT
    Tap,      // TAP x y
    Down,     // DOWN x y
    Move,     // MOVE x y
    Up,       // UP
    Invalid,  // unrecognized verb or malformed arguments
};

struct Command {
    Cmd cmd = Cmd::None;
    int16_t x = 0;
    int16_t y = 0;
};

// Parse a single command line (a NUL-terminated C string, without the trailing
// newline). Leading/trailing ASCII whitespace is ignored and the verb is
// matched case-insensitively. Coordinate arguments must be non-negative
// integers; anything malformed yields Cmd::Invalid so the caller can report it.
Command parse_command(const char* line);

}  // namespace bench
