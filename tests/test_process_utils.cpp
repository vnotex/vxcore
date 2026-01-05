#include <iostream>
#include <string>

#include "platform/process_utils.h"
#include "test_utils.h"

using namespace vxcore;

int test_is_command_available() {
  std::cout << "  Running test_is_command_available..." << std::endl;
#ifdef _WIN32
  ASSERT_TRUE(ProcessUtils::IsCommandAvailable("cmd"));
  ASSERT_TRUE(ProcessUtils::IsCommandAvailable("where"));
#else
  ASSERT_TRUE(ProcessUtils::IsCommandAvailable("sh"));
  ASSERT_TRUE(ProcessUtils::IsCommandAvailable("which"));
#endif

  ASSERT_FALSE(ProcessUtils::IsCommandAvailable("nonexistent_command_12345"));

  std::cout << "  ✓ test_is_command_available passed" << std::endl;
  return 0;
}

int test_execute_command_success() {
  std::cout << "  Running test_execute_command_success..." << std::endl;
#ifdef _WIN32
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("echo hello", result));
  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.exit_code, 0);
  ASSERT_TRUE(result.output.find("hello") != std::string::npos);
#else
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("echo hello", result));
  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.exit_code, 0);
  ASSERT_TRUE(result.output.find("hello") != std::string::npos);
#endif

  std::cout << "  ✓ test_execute_command_success passed" << std::endl;
  return 0;
}

int test_execute_command_failure() {
  std::cout << "  Running test_execute_command_failure..." << std::endl;
#ifdef _WIN32
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("cmd /c exit 1", result));
  ASSERT_FALSE(result.success);
  ASSERT_NE(result.exit_code, 0);
#else
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("sh -c 'exit 1'", result));
  ASSERT_FALSE(result.success);
  ASSERT_NE(result.exit_code, 0);
#endif

  std::cout << "  ✓ test_execute_command_failure passed" << std::endl;
  return 0;
}

int test_execute_command_with_output() {
  std::cout << "  Running test_execute_command_with_output..." << std::endl;
#ifdef _WIN32
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("echo line1 & echo line2", result));
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.output.find("line1") != std::string::npos);
  ASSERT_TRUE(result.output.find("line2") != std::string::npos);
#else
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand("echo line1; echo line2", result));
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.output.find("line1") != std::string::npos);
  ASSERT_TRUE(result.output.find("line2") != std::string::npos);
#endif

  std::cout << "  ✓ test_execute_command_with_output passed" << std::endl;
  return 0;
}

int test_escape_shell_arg_simple() {
  std::cout << "  Running test_escape_shell_arg_simple..." << std::endl;
  std::string arg = "hello";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);

#ifdef _WIN32
  ASSERT_EQ(escaped, "\"hello\"");
#else
  ASSERT_EQ(escaped, "'hello'");
#endif

  std::cout << "  ✓ test_escape_shell_arg_simple passed" << std::endl;
  return 0;
}

int test_escape_shell_arg_with_quotes() {
  std::cout << "  Running test_escape_shell_arg_with_quotes..." << std::endl;
#ifdef _WIN32
  std::string arg = "hello \"world\"";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "\"hello \\\"world\\\"\"");
#else
  std::string arg = "hello 'world'";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "'hello '\\''world'\\'''");
#endif

  std::cout << "  ✓ test_escape_shell_arg_with_quotes passed" << std::endl;
  return 0;
}

int test_escape_shell_arg_with_backslash() {
  std::cout << "  Running test_escape_shell_arg_with_backslash..." << std::endl;
#ifdef _WIN32
  std::string arg = "C:\\path\\to\\file";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "\"C:\\path\\to\\file\"");
#else
  std::string arg = "/path/to/file";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "'/path/to/file'");
#endif

  std::cout << "  ✓ test_escape_shell_arg_with_backslash passed" << std::endl;
  return 0;
}

int test_escape_shell_arg_special_chars() {
  std::cout << "  Running test_escape_shell_arg_special_chars..." << std::endl;
#ifdef _WIN32
  std::string arg = "test\"with\\special";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "\"test\\\"with\\special\"");
#else
  std::string arg = "test with spaces";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  ASSERT_EQ(escaped, "'test with spaces'");
#endif

  std::cout << "  ✓ test_escape_shell_arg_special_chars passed" << std::endl;
  return 0;
}

int test_execute_command_with_escaped_args() {
  std::cout << "  Running test_execute_command_with_escaped_args..." << std::endl;
#ifdef _WIN32
  std::string arg = "hello world";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  std::string command = "echo " + escaped;

  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand(command, result));
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.output.find("hello world") != std::string::npos);
#else
  std::string arg = "hello world";
  std::string escaped = ProcessUtils::EscapeShellArg(arg);
  std::string command = "echo " + escaped;

  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand(command, result));
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.output.find("hello world") != std::string::npos);
#endif

  std::cout << "  ✓ test_execute_command_with_escaped_args passed" << std::endl;
  return 0;
}

int test_open_close_pipe() {
  std::cout << "  Running test_open_close_pipe..." << std::endl;
#ifdef _WIN32
  FILE *pipe = ProcessUtils::OpenPipe("echo test", "r");
  ASSERT_TRUE(pipe != nullptr);

  char buffer[128];
  char *line = fgets(buffer, sizeof(buffer), pipe);
  ASSERT_TRUE(line != nullptr);
  ASSERT_TRUE(std::string(buffer).find("test") != std::string::npos);

  int exit_code = ProcessUtils::ClosePipe(pipe);
  ASSERT_EQ(exit_code, 0);
#else
  FILE *pipe = ProcessUtils::OpenPipe("echo test", "r");
  ASSERT_TRUE(pipe != nullptr);

  char buffer[128];
  char *line = fgets(buffer, sizeof(buffer), pipe);
  ASSERT_TRUE(line != nullptr);
  ASSERT_TRUE(std::string(buffer).find("test") != std::string::npos);

  int exit_code = ProcessUtils::ClosePipe(pipe);
  ASSERT_EQ(exit_code, 0);
#endif

  std::cout << "  ✓ test_open_close_pipe passed" << std::endl;
  return 0;
}

int test_utf8_input_output() {
  std::cout << "  Running test_utf8_input_output..." << std::endl;

  std::string utf8_text = "你好世界";
  std::string escaped = ProcessUtils::EscapeShellArg(utf8_text);

#ifdef _WIN32
  ASSERT_EQ(escaped, "\"你好世界\"");
  ASSERT_EQ(escaped.length(), 14);

  // UTF-8 in Windows cmd requires special handling
#else
  std::string command = "echo " + escaped;
  ProcessUtils::ProcessResult result;
  ASSERT_TRUE(ProcessUtils::ExecuteCommand(command, result));
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.output.find(utf8_text) != std::string::npos);
#endif

  std::cout << "  ✓ test_utf8_input_output passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running process_utils tests..." << std::endl;

  RUN_TEST(test_is_command_available);
  RUN_TEST(test_execute_command_success);
  RUN_TEST(test_execute_command_failure);
  RUN_TEST(test_execute_command_with_output);
  RUN_TEST(test_escape_shell_arg_simple);
  RUN_TEST(test_escape_shell_arg_with_quotes);
  RUN_TEST(test_escape_shell_arg_with_backslash);
  RUN_TEST(test_escape_shell_arg_special_chars);
  RUN_TEST(test_execute_command_with_escaped_args);
  RUN_TEST(test_open_close_pipe);
  RUN_TEST(test_utf8_input_output);

  std::cout << "✓ All process_utils tests passed" << std::endl;
  return 0;
}
