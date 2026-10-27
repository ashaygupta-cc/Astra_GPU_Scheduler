#pragma once

#include "astra/command.hpp"
#include <vector>
#include <string>
#include <string_view>

namespace astra {

class CommandParser {
public:
    static Command parse(const std::vector<std::string>& tokens, 
                         const std::string& workload_root = "./workloads");

    static Command parse_argc_argv(int argc, char* argv[], 
                                   const std::string& workload_root = "./workloads");

    static std::vector<std::string> tokenize(std::string_view input);

    static bool is_safe_workload_path(std::string_view path, 
                                      const std::string& workload_root, 
                                      std::string* error_out = nullptr);
};

} // namespace astra
