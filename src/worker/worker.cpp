#include "astra/worker.hpp"
#include "astra/ipc_channel.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

namespace astra {

namespace {

static volatile sig_atomic_t g_worker_stop = 0;

void handle_signal(int) {
    g_worker_stop = 1;
}

} // namespace

int run_worker_process(int worker_id, int ipc_fd) {
    // Reset signals in child process
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    IpcChannel channel{UniqueFd{ipc_fd}};

    while (!g_worker_stop && channel.is_open()) {
        MessageHeader header;
        std::string payload;

        // Block for command from scheduler parent
        if (!channel.receive(header, payload, -1)) {
            // Parent closed connection or EOF
            break;
        }

        if (header.type == static_cast<uint32_t>(MessageType::SHUTDOWN)) {
            break;
        }

        if (header.type == static_cast<uint32_t>(MessageType::HEARTBEAT)) {
            channel.send(MessageType::HEARTBEAT, 0, header.request_id, 0, "PONG");
            continue;
        }

        if (header.type == static_cast<uint32_t>(MessageType::RUN_JOB)) {
            auto job_json = nlohmann::json::parse(payload, nullptr, false);
            if (job_json.is_discarded()) {
                channel.send(MessageType::JOB_FAILED, header.job_id_numeric, header.request_id, -1, 
                             "{\"error\": \"Malformed RUN_JOB payload JSON\"}");
                continue;
            }

            std::string job_id = job_json.value("job_id", "");
            std::string executable = job_json.value("executable", "");
            std::vector<std::string> args = job_json.value("argv", std::vector<std::string>{});
            int timeout_seconds = job_json.value("timeout_seconds", 0);

            // Create pipe to capture child stdout/stderr
            int out_pipe[2] = {-1, -1};
            if (pipe(out_pipe) != 0) {
                channel.send(MessageType::JOB_FAILED, header.job_id_numeric, header.request_id, -1,
                             "{\"error\": \"Failed to create output pipe\"}");
                continue;
            }

            pid_t workload_pid = fork();
            if (workload_pid < 0) {
                close(out_pipe[0]);
                close(out_pipe[1]);
                channel.send(MessageType::JOB_FAILED, header.job_id_numeric, header.request_id, -1,
                             "{\"error\": \"Failed to fork child workload process\"}");
                continue;
            }

            if (workload_pid == 0) {
                // Child Workload Process
                close(out_pipe[0]);
                dup2(out_pipe[1], STDOUT_FILENO);
                dup2(out_pipe[1], STDERR_FILENO);
                close(out_pipe[1]);
                channel.close();

                // Prepare argv array
                std::string exec_target = executable;
                if (access(exec_target.c_str(), X_OK) != 0) {
                    std::string base = std::filesystem::path(executable).filename().string();
                    if (access(("build/" + executable).c_str(), X_OK) == 0) {
                        exec_target = "build/" + executable;
                    } else if (access(("build/workloads/" + base).c_str(), X_OK) == 0) {
                        exec_target = "build/workloads/" + base;
                    } else if (access(("workloads/" + base).c_str(), X_OK) == 0) {
                        exec_target = "workloads/" + base;
                    }
                }

                std::vector<char*> c_argv;
                c_argv.push_back(const_cast<char*>(exec_target.c_str()));
                for (auto& arg : args) {
                    c_argv.push_back(const_cast<char*>(arg.c_str()));
                }
                c_argv.push_back(nullptr);

                execvp(exec_target.c_str(), c_argv.data());
                // If exec fails:
                std::cerr << "Failed to exec workload binary: " << executable << " (target: " << exec_target << ")\n";
                _exit(127);
            }

            // In Worker Supervisor
            close(out_pipe[1]);
            UniqueFd read_out(out_pipe[0]);

            // Set read pipe non-blocking
            int flags = fcntl(read_out.get(), F_GETFL, 0);
            fcntl(read_out.get(), F_SETFL, flags | O_NONBLOCK);

            // Report JOB_STARTED to parent
            nlohmann::json start_info{
                {"job_id", job_id},
                {"workload_pid", workload_pid},
                {"worker_id", worker_id}
            };
            channel.send(MessageType::JOB_STARTED, header.job_id_numeric, header.request_id, 0, start_info.dump());

            std::string captured_output;
            char read_buf[4096];
            bool workload_running = true;
            int final_exit_code = 0;
            auto start_time = std::chrono::steady_clock::now();

            while (workload_running && !g_worker_stop) {
                // 1. Check if timeout exceeded
                if (timeout_seconds > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_seconds) {
                        kill(workload_pid, SIGTERM);
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        kill(workload_pid, SIGKILL);
                    }
                }

                // 2. Poll IPC socket for CANCEL_JOB and output pipe
                struct pollfd pfds[2];
                pfds[0].fd = channel.fd();
                pfds[0].events = POLLIN;
                pfds[1].fd = read_out.get();
                pfds[1].events = POLLIN;

                int poll_rc = poll(pfds, 2, 50); // 50ms interval
                if (poll_rc > 0) {
                    // Check parent message on IPC
                    if (pfds[0].revents & POLLIN) {
                        MessageHeader cancel_hdr;
                        std::string cancel_payload;
                        if (channel.receive(cancel_hdr, cancel_payload, 0)) {
                            if (cancel_hdr.type == static_cast<uint32_t>(MessageType::CANCEL_JOB)) {
                                kill(workload_pid, SIGTERM);
                                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                kill(workload_pid, SIGKILL);
                            }
                        }
                    }

                    // Check output on pipe
                    if (pfds[1].revents & POLLIN) {
                        ssize_t n = read(read_out.get(), read_buf, sizeof(read_buf));
                        if (n > 0 && captured_output.size() < 64 * 1024) {
                            captured_output.append(read_buf, static_cast<size_t>(n));
                        }
                    }
                }

                // 3. Check child exit status
                int status = 0;
                pid_t wait_res = waitpid(workload_pid, &status, WNOHANG);
                if (wait_res == workload_pid) {
                    workload_running = false;
                    if (WIFEXITED(status)) {
                        final_exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        final_exit_code = -WTERMSIG(status);
                    }
                } else if (wait_res < 0) {
                    workload_running = false;
                    final_exit_code = -1;
                }
            }

            // Drain remaining output
            while (true) {
                ssize_t n = read(read_out.get(), read_buf, sizeof(read_buf));
                if (n > 0 && captured_output.size() < 64 * 1024) {
                    captured_output.append(read_buf, static_cast<size_t>(n));
                } else {
                    break;
                }
            }

            // Send completion or failure
            nlohmann::json result_info{
                {"job_id", job_id},
                {"workload_pid", workload_pid},
                {"exit_code", final_exit_code},
                {"output", captured_output}
            };

            if (final_exit_code == 0) {
                channel.send(MessageType::JOB_COMPLETED, header.job_id_numeric, header.request_id, 
                             0, result_info.dump());
            } else {
                channel.send(MessageType::JOB_FAILED, header.job_id_numeric, header.request_id, 
                             final_exit_code, result_info.dump());
            }
        }
    }

    return 0;
}

} // namespace astra
