#ifndef VXCORE_PROCESS_UTILS_H
#define VXCORE_PROCESS_UTILS_H

#include <cstdio>
#include <string>

namespace vxcore {

class ProcessUtils {
 public:
  struct ProcessResult {
    std::string output;
    int exit_code;
    bool success;
  };

  static FILE *OpenPipe(const std::string &command, const char *mode);

  static int ClosePipe(FILE *pipe);

  static bool ExecuteCommand(const std::string &command, ProcessResult &result);

  static bool IsCommandAvailable(const std::string &command_name);

  static std::string EscapeShellArg(const std::string &arg);
};

}  // namespace vxcore

#endif
