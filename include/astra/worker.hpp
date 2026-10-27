#pragma once

#include "astra/ipc_channel.hpp"
#include <string>
#include <vector>

namespace astra {

// Entry point for worker process executed after fork()
int run_worker_process(int worker_id, int ipc_fd);

} // namespace astra
