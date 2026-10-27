#include <unity.h>
#include <cstring>
#include "utils.h"

void setUp(void) {}
void tearDown(void) {}

// --- uvColor ---
void test_uv_below_1()      { TEST_ASSERT_EQUAL_HEX(0x658D1B, uvColor(0.0f)); }
void test_uv_boundary_1()   { TEST_ASSERT_EQUAL_HEX(0x84BD00, uvColor(1.0f)); }
void test_uv_boundary_5()   { TEST_ASSERT_EQUAL_HEX(0xFFCD00, uvColor(5.0f)); }
void test_uv_extreme()      { TEST_ASSERT_EQUAL_HEX(0x4B1E88, uvColor(11.0f)); }
void test_uv_very_high()    { TEST_ASSERT_EQUAL_HEX(0x4B1E88, uvColor(15.0f)); }

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

// --- crc32Calc ---
void test_crc_empty() {
    TEST_ASSERT_EQUAL_HEX32(0x00000000, crc32Calc(nullptr, 0));
}
void test_crc_check_value() {
    // The standard CRC-32 (zlib/IEEE) check value
    const char* d = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32Calc(d, strlen(d)));
}
void test_crc_detects_swap() {
    // Reordered bytes - the old XOR checksum gave the same result for both
    uint8_t a[] = {0x01, 0x02};
    uint8_t b[] = {0x02, 0x01};
    TEST_ASSERT_NOT_EQUAL(crc32Calc(a, 2), crc32Calc(b, 2));
}

// --- gridPercentage ---
void test_grid_no_usage()   { TEST_ASSERT_EQUAL(-1,  gridPercentage(0.0f, 0.0f)); }
void test_grid_all_solar()  { TEST_ASSERT_EQUAL(0,   gridPercentage(0.0f, 10.0f)); }
void test_grid_half()       { TEST_ASSERT_EQUAL(50,  gridPercentage(5.0f, 10.0f)); }
void test_grid_rounds()     { TEST_ASSERT_EQUAL(1,   gridPercentage(0.06f, 10.0f)); } // 0.6% rounds up, not down to 0
void test_grid_clamps_high(){ TEST_ASSERT_EQUAL(100, gridPercentage(12.0f, 10.0f)); }
void test_grid_clamps_low() { TEST_ASSERT_EQUAL(0,   gridPercentage(-1.0f, 10.0f)); }

// --- formatIntegerWithCommas ---
void test_fmt_zero() {
    char buf[32];
    formatIntegerWithCommas(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0", buf);
}
void test_fmt_small() {
    char buf[32];
    formatIntegerWithCommas(999, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("999", buf);
}
void test_fmt_thousands() {
    char buf[32];
    formatIntegerWithCommas(1000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1,000", buf);
}
void test_fmt_millions() {
    char buf[32];
    formatIntegerWithCommas(1234567, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1,234,567", buf);
}
void test_fmt_negative() {
    char buf[32];
    formatIntegerWithCommas(-1000, buf, sizeof(buf));
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

    RUN_TEST(test_crc_empty);
    RUN_TEST(test_crc_check_value);
    RUN_TEST(test_crc_detects_swap);

    RUN_TEST(test_grid_no_usage);
    RUN_TEST(test_grid_all_solar);
    RUN_TEST(test_grid_half);
    RUN_TEST(test_grid_rounds);
    RUN_TEST(test_grid_clamps_high);
    RUN_TEST(test_grid_clamps_low);

    RUN_TEST(test_fmt_zero);
    RUN_TEST(test_fmt_small);
    RUN_TEST(test_fmt_thousands);
    RUN_TEST(test_fmt_millions);
    RUN_TEST(test_fmt_negative);

    return UNITY_END();
}
