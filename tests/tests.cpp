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

  static const std::deque<Message> &s_get_messages() { return s_messages; }
};

TEST_CASE("normalises message line endings") {
  CHECK(ConsoleWindowTestImpl::s_process_message("Test"sv).m_message ==
        "Test"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\r"sv).m_message ==
        "Test"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\n"sv).m_message ==
        "Test"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\r\n"sv).m_message ==
        "Test"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\rTest"sv).m_message ==
        "TestTest"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\nTest"sv).m_message ==
        "Test\r\nTest"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\r\nTest"sv).m_message ==
        "Test\r\nTest"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\r\rTest"sv).m_message ==
        "TestTest"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\n\nTest"sv).m_message ==
        "Test\r\n\r\nTest"sv);
  CHECK(ConsoleWindowTestImpl::s_process_message("Test\r\n\r\nTest"sv)
            .m_message == "Test\r\n\r\nTest"sv);
}
