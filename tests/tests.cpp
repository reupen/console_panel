#include "pch.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace std::string_view_literals;

class ConsoleWindowTestImpl
    : public window_implementation<ConsoleWindow, false> {
public:
  static Message s_process_message(std::string_view text) {
    s_on_message_received(text);
    Message message = s_messages.back();
    s_messages.pop_back();
    return message;
  }
};

auto normalise_message(std::string_view text) {
  return ConsoleWindowTestImpl::s_process_message(text).m_message;
}

TEST_CASE("normalises message line endings") {
  CHECK(normalise_message("Test"sv) == "Test"sv);
  CHECK(normalise_message("Test\r"sv) == "Test"sv);
  CHECK(normalise_message("Test\n"sv) == "Test"sv);
  CHECK(normalise_message("Test\r\n"sv) == "Test"sv);
  CHECK(normalise_message("Test\rTest"sv) == "TestTest"sv);
  CHECK(normalise_message("Test\nTest"sv) == "Test\r\nTest"sv);
  CHECK(normalise_message("Test\r\nTest"sv) == "Test\r\nTest"sv);
  CHECK(normalise_message("Test\r\rTest"sv) == "TestTest"sv);
  CHECK(normalise_message("Test\n\nTest"sv) == "Test\r\n\r\nTest"sv);
  CHECK(normalise_message("Test\r\n\r\nTest"sv) == "Test\r\n\r\nTest"sv);
}
