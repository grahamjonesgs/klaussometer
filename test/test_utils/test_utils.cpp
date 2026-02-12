#include <unity.h>
#include "utils.h"

void setUp(void) {}
void tearDown(void) {}

// --- uv_color ---
void test_uv_below_1()      { TEST_ASSERT_EQUAL_HEX(0x658D1B, uv_color(0.0f)); }
void test_uv_boundary_1()   { TEST_ASSERT_EQUAL_HEX(0x84BD00, uv_color(1.0f)); }
void test_uv_boundary_5()   { TEST_ASSERT_EQUAL_HEX(0xFFCD00, uv_color(5.0f)); }
void test_uv_extreme()      { TEST_ASSERT_EQUAL_HEX(0x4B1E88, uv_color(11.0f)); }
void test_uv_very_high()    { TEST_ASSERT_EQUAL_HEX(0x4B1E88, uv_color(15.0f)); }

// --- degreesToDirection ---
void test_dir_north()       { TEST_ASSERT_EQUAL_STRING("N",  degreesToDirection(0.0)); }
void test_dir_east()        { TEST_ASSERT_EQUAL_STRING("E",  degreesToDirection(90.0)); }
void test_dir_south()       { TEST_ASSERT_EQUAL_STRING("S",  degreesToDirection(180.0)); }
void test_dir_west()        { TEST_ASSERT_EQUAL_STRING("W",  degreesToDirection(270.0)); }
void test_dir_northeast()   { TEST_ASSERT_EQUAL_STRING("NE", degreesToDirection(45.0)); }
void test_dir_360_is_north(){ TEST_ASSERT_EQUAL_STRING("N",  degreesToDirection(360.0)); }
void test_dir_negative()    { TEST_ASSERT_EQUAL_STRING("W",  degreesToDirection(-90.0)); }

// --- wmoToText ---
void test_wmo_sunny_day()   { TEST_ASSERT_EQUAL_STRING("Sunny",         wmoToText(0, true)); }
void test_wmo_clear_night() { TEST_ASSERT_EQUAL_STRING("Clear",         wmoToText(0, false)); }
void test_wmo_overcast()    { TEST_ASSERT_EQUAL_STRING("Overcast",      wmoToText(3, true)); }
void test_wmo_thunderstorm(){ TEST_ASSERT_EQUAL_STRING("Thunderstorm",  wmoToText(95, true)); }
void test_wmo_unknown()     { TEST_ASSERT_EQUAL_STRING("Unknown weather code", wmoToText(999, true)); }

// --- compareVersionsStr ---
void test_ver_greater()     { TEST_ASSERT_EQUAL( 1, compareVersionsStr("4.1.36", "4.1.35")); }
void test_ver_less()        { TEST_ASSERT_EQUAL(-1, compareVersionsStr("4.1.34", "4.1.35")); }
void test_ver_equal()       { TEST_ASSERT_EQUAL( 0, compareVersionsStr("4.1.35", "4.1.35")); }
void test_ver_major_wins()  { TEST_ASSERT_EQUAL( 1, compareVersionsStr("5.0.0",  "4.9.9")); }
void test_ver_minor_wins()  { TEST_ASSERT_EQUAL(-1, compareVersionsStr("4.0.9",  "4.1.0")); }

// --- calculateChecksum ---
void test_csum_empty() {
    TEST_ASSERT_EQUAL_HEX(0x00, calculateChecksum(nullptr, 0));
}
void test_csum_single() {
    uint8_t d[] = {0x42};
    TEST_ASSERT_EQUAL_HEX(0x42, calculateChecksum(d, 1));
}
void test_csum_xor() {
    uint8_t d[] = {0xAA, 0xBB, 0xAA};  // 0xAA^0xBB^0xAA = 0xBB
    TEST_ASSERT_EQUAL_HEX(0xBB, calculateChecksum(d, 3));
}
void test_csum_self_cancels() {
    uint8_t d[] = {0xFF, 0xFF};
    TEST_ASSERT_EQUAL_HEX(0x00, calculateChecksum(d, 2));
}

// --- format_integer_with_commas ---
void test_fmt_zero() {
    char buf[32];
    format_integer_with_commas(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0", buf);
}
void test_fmt_small() {
    char buf[32];
    format_integer_with_commas(999, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("999", buf);
}
void test_fmt_thousands() {
    char buf[32];
    format_integer_with_commas(1000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1,000", buf);
}
void test_fmt_millions() {
    char buf[32];
    format_integer_with_commas(1234567, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1,234,567", buf);
}
void test_fmt_negative() {
    char buf[32];
    format_integer_with_commas(-1000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("-1,000", buf);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_uv_below_1);
    RUN_TEST(test_uv_boundary_1);
    RUN_TEST(test_uv_boundary_5);
    RUN_TEST(test_uv_extreme);
    RUN_TEST(test_uv_very_high);

    RUN_TEST(test_dir_north);
    RUN_TEST(test_dir_east);
    RUN_TEST(test_dir_south);
    RUN_TEST(test_dir_west);
    RUN_TEST(test_dir_northeast);
    RUN_TEST(test_dir_360_is_north);
    RUN_TEST(test_dir_negative);

    RUN_TEST(test_wmo_sunny_day);
    RUN_TEST(test_wmo_clear_night);
    RUN_TEST(test_wmo_overcast);
    RUN_TEST(test_wmo_thunderstorm);
    RUN_TEST(test_wmo_unknown);

    RUN_TEST(test_ver_greater);
    RUN_TEST(test_ver_less);
    RUN_TEST(test_ver_equal);
    RUN_TEST(test_ver_major_wins);
    RUN_TEST(test_ver_minor_wins);

    RUN_TEST(test_csum_empty);
    RUN_TEST(test_csum_single);
    RUN_TEST(test_csum_xor);
    RUN_TEST(test_csum_self_cancels);

    RUN_TEST(test_fmt_zero);
    RUN_TEST(test_fmt_small);
    RUN_TEST(test_fmt_thousands);
    RUN_TEST(test_fmt_millions);
    RUN_TEST(test_fmt_negative);

    return UNITY_END();
}
