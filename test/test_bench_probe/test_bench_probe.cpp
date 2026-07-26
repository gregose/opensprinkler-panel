#include <unity.h>

#include "bench_probe.h"

using bench::Cmd;
using bench::Command;
using bench::parse_command;

void setUp() {}
void tearDown() {}

static void assert_cmd(const char* line, Cmd cmd, int x, int y) {
    Command c = parse_command(line);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cmd), static_cast<int>(c.cmd));
    if (cmd == Cmd::Tap || cmd == Cmd::Down || cmd == Cmd::Move) {
        TEST_ASSERT_EQUAL_INT(x, c.x);
        TEST_ASSERT_EQUAL_INT(y, c.y);
    }
}

static void test_shot() {
    assert_cmd("SHOT", Cmd::Shot, 0, 0);
}

static void test_shot_case_insensitive() {
    assert_cmd("shot", Cmd::Shot, 0, 0);
    assert_cmd("ShOt", Cmd::Shot, 0, 0);
}

static void test_up() {
    assert_cmd("UP", Cmd::Up, 0, 0);
    assert_cmd("up", Cmd::Up, 0, 0);
}

static void test_tap() {
    assert_cmd("TAP 120 200", Cmd::Tap, 120, 200);
}

static void test_tap_lowercase() {
    assert_cmd("tap 5 6", Cmd::Tap, 5, 6);
}

static void test_down_move() {
    assert_cmd("DOWN 0 0", Cmd::Down, 0, 0);
    assert_cmd("MOVE 479 319", Cmd::Move, 479, 319);
}

static void test_leading_and_trailing_whitespace() {
    assert_cmd("   TAP   10    20   ", Cmd::Tap, 10, 20);
    assert_cmd("  SHOT  ", Cmd::Shot, 0, 0);
}

static void test_blank_line_is_none() {
    assert_cmd("", Cmd::None, 0, 0);
    assert_cmd("    ", Cmd::None, 0, 0);
}

static void test_null_is_none() {
    Command c = parse_command(nullptr);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Cmd::None), static_cast<int>(c.cmd));
}

static void test_unknown_verb_invalid() {
    assert_cmd("WIGGLE 1 2", Cmd::Invalid, 0, 0);
    assert_cmd("SHOTX", Cmd::Invalid, 0, 0);
}

static void test_tap_missing_args_invalid() {
    assert_cmd("TAP", Cmd::Invalid, 0, 0);
    assert_cmd("TAP 10", Cmd::Invalid, 0, 0);
}

static void test_tap_nonnumeric_invalid() {
    assert_cmd("TAP a b", Cmd::Invalid, 0, 0);
    assert_cmd("TAP 10 x", Cmd::Invalid, 0, 0);
}

static void test_tap_trailing_junk_invalid() {
    assert_cmd("TAP 10 20 30", Cmd::Invalid, 0, 0);
    assert_cmd("TAP 10 20 foo", Cmd::Invalid, 0, 0);
}

static void test_shot_with_args_invalid() {
    assert_cmd("SHOT now", Cmd::Invalid, 0, 0);
    assert_cmd("UP 1", Cmd::Invalid, 0, 0);
}

static void test_negative_coord_invalid() {
    // The parser only accepts non-negative decimals; a leading '-' is junk.
    assert_cmd("TAP -1 5", Cmd::Invalid, 0, 0);
}

static void test_coord_overflow_invalid() {
    assert_cmd("TAP 40000 5", Cmd::Invalid, 0, 0);
}

static void test_max_coord_ok() {
    assert_cmd("MOVE 32767 32767", Cmd::Move, 32767, 32767);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_shot);
    RUN_TEST(test_shot_case_insensitive);
    RUN_TEST(test_up);
    RUN_TEST(test_tap);
    RUN_TEST(test_tap_lowercase);
    RUN_TEST(test_down_move);
    RUN_TEST(test_leading_and_trailing_whitespace);
    RUN_TEST(test_blank_line_is_none);
    RUN_TEST(test_null_is_none);
    RUN_TEST(test_unknown_verb_invalid);
    RUN_TEST(test_tap_missing_args_invalid);
    RUN_TEST(test_tap_nonnumeric_invalid);
    RUN_TEST(test_tap_trailing_junk_invalid);
    RUN_TEST(test_shot_with_args_invalid);
    RUN_TEST(test_negative_coord_invalid);
    RUN_TEST(test_coord_overflow_invalid);
    RUN_TEST(test_max_coord_ok);
    return UNITY_END();
}
