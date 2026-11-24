#include <unity.h>

#include <espurna/types.h>
#include <espurna/libs/Delimiter.h>

namespace {

espurna::string printable_string(espurna::StringView view) {
    espurna::string out;
    out.reserve(view.length());

    for (auto c : view) {
        if (isprint(c)) {
            out += c;
        } else {
            out += "\\x";
            out += espurna::string(int(c), 16);
        }
    }

    return out;
}

espurna::string expected_actual(espurna::StringView expected, espurna::StringView actual) {
    espurna::string out;

    out += "'";
    out += expected;

    out += "' ";
    out += "(" + espurna::string(expected.length(), 10) + ") vs. ";

    out += "'";
    out += printable_string(actual);
    out += "' ";
    out += "(" + espurna::string(actual.length(), 10) + ")";

    return out;
}

} // namespace

void test_assert_equal_string_view(espurna::StringView expected, espurna::StringView actual, unsigned int line, const char* msg) {
    espurna::string replacement;
    if (msg == nullptr) {
        replacement = expected_actual(expected, actual);
    }

    UNITY_TEST_ASSERT_EQUAL_INT(expected.length(), actual.length(),
        line, ("length is different, " + (replacement.length() ? replacement : espurna::string(msg))).c_str());
    UNITY_TEST_ASSERT_EQUAL_CHAR_ARRAY(
        expected.data(), actual.data(), actual.length(), line, replacement.length() ? replacement.c_str() : msg);
}
