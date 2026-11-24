#include <unity.h>


#include <espurna/filters/LastFilter.h>
#include <espurna/filters/MaxFilter.h>
#include <espurna/filters/MedianFilter.h>
#include <espurna/filters/MinFilter.h>
#include <espurna/filters/MovingAverageFilter.h>
#include <espurna/filters/SumFilter.h>

#include <algorithm>

namespace espurna {
namespace test {
namespace {

void test_last() {
    auto filter = LastFilter();

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.resize(123);

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    const double samples[] = {123.4, 456.7, 789.0, 1.0};
    for (auto& sample : samples) {
        filter.update(sample);
        TEST_ASSERT(filter.available());
        TEST_ASSERT(filter.ready());
        TEST_ASSERT_EQUAL_DOUBLE(sample, filter.value());
    }

    filter.reset();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(111.11);
    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());

    filter.restart();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());
}

void test_max() {
    auto filter = MaxFilter();

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.resize(567);
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

#define TEST_EXPECTED(EXPECTED, VALUE)\
    filter.update(VALUE);\
    TEST_ASSERT(filter.available());\
    TEST_ASSERT(filter.ready());\
    TEST_ASSERT_EQUAL_DOUBLE(EXPECTED, filter.value());\

    TEST_EXPECTED(5.0, 5.0);
    TEST_EXPECTED(10.0, 10.0);
    TEST_EXPECTED(15.0, 15.0);
    TEST_EXPECTED(15.0, 10.0);
    TEST_EXPECTED(15.0, -10.0);
    TEST_EXPECTED(15.0, -15.0);
    TEST_EXPECTED(15.0, 0.0);
    TEST_EXPECTED(30.0, 30.0);

#undef TEST_EXPECTED

    filter.reset();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(123.0);
    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(123.0, filter.value());

    filter.restart();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(567.0);
    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(567.0, filter.value());
}

void test_median() {
    auto filter = MedianFilter();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    const double one[] {4., 3., 5., 6., 2., 2., 3., 4., 7., 9.};
    filter.resize(std::size(one));

    auto it = std::begin(one);
    filter.update(*it);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(!filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(4., filter.value());

    ++it;
    filter.update(*it);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(!filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(3.5, filter.value());

    ++it;
    while (it != std::end(one)) {
        filter.update(*it);
        ++it;
    }

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(4.0, filter.value());

    const double two[] {6., 6.1, 6.2, 6.3, 6.4, 6.5, 2.5, 4.5, 2.6, 2.5, 2.4};

    static_assert(std::size(one) < std::size(two), "");
    filter.resize(std::size(two));

    TEST_ASSERT(filter.available());
    TEST_ASSERT(!filter.ready());

    TEST_ASSERT_EQUAL_DOUBLE(4.0, filter.value()); // previous median, since total size increased

    for (const auto& sample : two) {
        filter.update(sample);
    }

    TEST_ASSERT_EQUAL_DOUBLE(6.0, filter.value());

    const double three[] {2.4, 2.4};

    static_assert(std::size(three) < std::size(two), "");
    filter.resize(std::size(three));

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(2.45, filter.value()); // median([2.5, 2.4])

    for (const auto& sample : three) {
        filter.update(sample);
    }

    TEST_ASSERT_EQUAL_DOUBLE(2.4, filter.value()); // median([2.4, 2.4])

    filter.resize(1);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(2.4, filter.value()); // median([2.4])

    filter.update(4.8);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(4.8, filter.value()); // median([4.8])

    filter.update(6.51);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(6.51, filter.value()); // median([6.51])

    filter.resize(0);

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(5.31);

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());
}

void test_min() {
    auto filter = MinFilter();

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.resize(999);
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

#define TEST_EXPECTED(EXPECTED, VALUE)\
    filter.update(VALUE);\
    TEST_ASSERT(filter.available());\
    TEST_ASSERT(filter.ready());\
    TEST_ASSERT_EQUAL_DOUBLE(EXPECTED, filter.value());\

    TEST_EXPECTED(100.0, 100.0);
    TEST_EXPECTED(90.0, 90.0);
    TEST_EXPECTED(90.0, 110.0);
    TEST_EXPECTED(80.0, 80.0);
    TEST_EXPECTED(-100.0, -100.0);
    TEST_EXPECTED(-100.0, 200.0);
    TEST_EXPECTED(-100.0, 0.0);
    TEST_EXPECTED(-200.0, -200.0);

#undef TEST_EXPECTED

    filter.reset();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(44.0);
    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(44.0, filter.value());

    filter.restart();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    filter.update(45.0);
    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());
    TEST_ASSERT_EQUAL_DOUBLE(45.0, filter.value());
}

void test_moving_average() {
    auto filter = MovingAverageFilter();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    const double one[] {22., 22.3, 22.1, 22.1, 22.1, 22.0, 22.5, 22.1};
    filter.resize(std::size(one));

    auto it = std::begin(one);
    filter.update(*it);

    TEST_ASSERT(!filter.ready());
    TEST_ASSERT(filter.available());

    ++it;
    while (it != std::end(one)) {
        filter.update(*it);
        ++it;
    }

    TEST_ASSERT(filter.ready());
    TEST_ASSERT(filter.available());

    TEST_ASSERT_EQUAL_DOUBLE(22.15, filter.value());

    const double two[] {5., 6., 7., 8., 9., 10., 11., 12., 13.};
    static_assert(std::size(one) < std::size(two), "");

    filter.resize(std::size(two));
    TEST_ASSERT(!filter.ready());
    TEST_ASSERT(filter.available());

    for (const auto& sample : two) {
        filter.update(sample);
    }

    TEST_ASSERT(filter.ready());
    TEST_ASSERT(filter.available());

    TEST_ASSERT_EQUAL_DOUBLE(9.0, filter.value());

    const double three[] {14., 15., 16., 17.};
    static_assert(std::size(three) < std::size(two), "");

    for (const auto& sample : three) {
        filter.update(sample);
    }

    TEST_ASSERT(filter.ready());
    TEST_ASSERT(filter.available());

    TEST_ASSERT_EQUAL_DOUBLE(13.0, filter.value());

    filter.resize(1);

    TEST_ASSERT(filter.ready());
    TEST_ASSERT(filter.available());

    TEST_ASSERT_EQUAL_DOUBLE(17.0, filter.value());

    filter.update(5.31);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());

    TEST_ASSERT_EQUAL_DOUBLE(5.31, filter.value());

    filter.resize(0);

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());
}

void test_sum() {
    auto filter = SumFilter();

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    const double one[] {20., 20.1, 13., 10., 5., 14., 29., 32.};
    filter.resize(std::size(one));

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    for (const auto& sample : one) {
        filter.update(sample);
    }

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());

    TEST_ASSERT_EQUAL_DOUBLE(143.1, filter.value());

    filter.reset();
    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    const double two[] {-15.0, 30.0, -15.0, 10.0, 1.0, 3.0};
    filter.resize(std::size(two));

    TEST_ASSERT(!filter.available());
    TEST_ASSERT(!filter.ready());

    auto it = std::begin(two);
    filter.update(*it);

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());

    ++it;
    while (it != std::end(two)) {
        filter.update(*it);
        ++it;
    }

    TEST_ASSERT(filter.available());
    TEST_ASSERT(filter.ready());

    TEST_ASSERT_EQUAL_DOUBLE(14.0, filter.value());
}

} // namespace
} // namespace test
} // namespace espurna

int main(int, char**) {
    UNITY_BEGIN();
    using namespace espurna::test;
    RUN_TEST(test_last);
    RUN_TEST(test_max);
    RUN_TEST(test_median);
    RUN_TEST(test_min);
    RUN_TEST(test_moving_average);
    RUN_TEST(test_sum);
    return UNITY_END();
}

