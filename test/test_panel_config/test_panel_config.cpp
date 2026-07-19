// Native tests for provisioning/config normalization helpers.
#include <unity.h>

#include <string>

#include "panel_config.h"

using namespace osp;

void setUp() {}
void tearDown() {}

void test_trim_ascii() {
  TEST_ASSERT_EQUAL_STRING("abc", trim_ascii("  abc\t\n").c_str());
  TEST_ASSERT_EQUAL_STRING("", trim_ascii(" \r\n ").c_str());
}

void test_normalize_os_host_trims_protocol_and_slashes() {
  TEST_ASSERT_EQUAL_STRING("controller.local",
                           normalize_os_host(" https://controller.local/ ").c_str());
  TEST_ASSERT_EQUAL_STRING("192.168.1.50",
                           normalize_os_host("http://192.168.1.50//").c_str());
}

void test_normalize_md5_hex_lowercases() {
  TEST_ASSERT_EQUAL_STRING("a6d82bced638de3def1e9bbb4983225c",
                           normalize_md5_hex(" A6D82BCED638DE3DEF1E9BBB4983225C ").c_str());
}

void test_is_valid_md5_hex() {
  TEST_ASSERT_TRUE(is_valid_md5_hex("a6d82bced638de3def1e9bbb4983225c"));
  TEST_ASSERT_TRUE(is_valid_md5_hex("A6D82BCED638DE3DEF1E9BBB4983225C"));
  TEST_ASSERT_FALSE(is_valid_md5_hex("short"));
  TEST_ASSERT_FALSE(is_valid_md5_hex("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));
}

void test_has_provisioning_config() {
  TEST_ASSERT_TRUE(has_provisioning_config(
      "MyWiFi", "http://controller.local/", "a6d82bced638de3def1e9bbb4983225c"));
  TEST_ASSERT_FALSE(has_provisioning_config(
      "", "controller.local", "a6d82bced638de3def1e9bbb4983225c"));
  TEST_ASSERT_FALSE(has_provisioning_config(
      "MyWiFi", "", "a6d82bced638de3def1e9bbb4983225c"));
  TEST_ASSERT_FALSE(has_provisioning_config(
      "MyWiFi", "controller.local", "bad"));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_trim_ascii);
  RUN_TEST(test_normalize_os_host_trims_protocol_and_slashes);
  RUN_TEST(test_normalize_md5_hex_lowercases);
  RUN_TEST(test_is_valid_md5_hex);
  RUN_TEST(test_has_provisioning_config);

  return UNITY_END();
}
