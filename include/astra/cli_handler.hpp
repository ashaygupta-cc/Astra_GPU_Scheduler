#pragma once

#include "astra/command.hpp"
#include "astra/job_store.hpp"
#include "astra/config.hpp"

#include <iostream>
#include <string>

namespace astra {

class CliHandler {
public:
    CliHandler(JobStore& store, AstraConfig& config);

    int execute(const Command& cmd, std::ostream& out = std::cout, std::ostream& err = std::cerr);

    int run_repl(std::istream& in = std::cin, std::ostream& out = std::cout, std::ostream& err = std::cerr);

private:
    JobStore& store_;
    AstraConfig& config_;

    std::string generate_job_id();
};

} // namespace astra
