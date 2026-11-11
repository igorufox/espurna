#include <Arduino.h>
#include <unity.h>

#include <espurna/modules_package.h>

using namespace espurna::modules::package;

namespace {

constexpr uint32_t AreaSize { 512 * 1024 };

void test_round_up() {
    TEST_ASSERT_EQUAL_UINT32(0, roundUp(0, 4096));
    TEST_ASSERT_EQUAL_UINT32(4096, roundUp(4, 4096));
    TEST_ASSERT_EQUAL_UINT32(4096, roundUp(4096, 4096));
    TEST_ASSERT_EQUAL_UINT32(8192, roundUp(4097, 4096));
    // used to wrap to 0
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, roundUp(0xFFFFF004, 4096));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, roundUp(UINT32_MAX, 8));
}

void test_sizes() {
    TEST_ASSERT(Sizes::BadImage == sizesCheck(0, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::BadImage == sizesCheck(6, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::Ok == sizesCheck(4, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::Ok == sizesCheck(AreaSize, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::ImageTooLarge == sizesCheck(AreaSize + 4, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::ImageTooLarge == sizesCheck(0xFFFFF004, 0, 0, AreaSize));
    TEST_ASSERT(Sizes::ImageTooLarge == sizesCheck(4, 0, 0, 0));
    TEST_ASSERT(Sizes::Ok == sizesCheck(4, RamLimit, RamLimit, AreaSize));
    TEST_ASSERT(Sizes::BadRam == sizesCheck(4, RamLimit + 1, 0, AreaSize));
    TEST_ASSERT(Sizes::BadRam == sizesCheck(4, 0, 0xFFFFFFF8, AreaSize));
}

void test_inside() {
    TEST_ASSERT(inside(0x1000, 0x1000, 0x1000, 0x3000));
    TEST_ASSERT(inside(0x2000, 0x1000, 0x1000, 0x3000));
    TEST_ASSERT(inside(0x3000, 0, 0x1000, 0x3000));
    TEST_ASSERT(!inside(0x2000, 0x1001, 0x1000, 0x3000));
    TEST_ASSERT(!inside(0x0FFF, 4, 0x1000, 0x3000));
    // offset + size wraps past 4 GiB
    TEST_ASSERT(!inside(0x2000, 0xFFFFF000, 0x1000, 0x3000));
    TEST_ASSERT(!inside(0x1000, 0, 0x3000, 0x1000));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_round_up);
    RUN_TEST(test_sizes);
    RUN_TEST(test_inside);
    return UNITY_END();
}
