#include "astra/ipc_channel.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <iostream>

namespace astra {

IpcChannel::IpcChannel(UniqueFd fd) : fd_(std::move(fd)) {}

std::pair<UniqueFd, UniqueFd> IpcChannel::create_socketpair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {UniqueFd(-1), UniqueFd(-1)};
    }
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

bool IpcChannel::write_exact(const void* buf, size_t count) {
    if (!fd_.valid()) return false;

    const char* ptr = static_cast<const char*>(buf);
    size_t total_written = 0;

    while (total_written < count) {
        ssize_t n = ::write(fd_.get(), ptr + total_written, count - total_written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            // Channel closed
            return false;
        }
        total_written += static_cast<size_t>(n);
    }
    return true;
}

bool IpcChannel::read_exact(void* buf, size_t count, int timeout_ms) {
    if (!fd_.valid()) return false;

    char* ptr = static_cast<char*>(buf);
    size_t total_read = 0;

    while (total_read < count) {
        if (timeout_ms >= 0) {
            struct pollfd pfd{};
            pfd.fd = fd_.get();
            pfd.events = POLLIN;

            int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (rc == 0) {
                // Timeout
                return false;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd.revents & POLLIN)) {
                return false;
            }
        }

        ssize_t n = ::read(fd_.get(), ptr + total_read, count - total_read);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            // EOF: peer closed the socket
            return false;
        }
        total_read += static_cast<size_t>(n);
    }
    return true;
}

bool IpcChannel::send(MessageType type, uint64_t job_id_num, uint32_t req_id, 
                      int32_t status_code, const std::string& payload) {
    if (payload.size() > MAX_PAYLOAD_SIZE) {
        return false;
    }

    MessageHeader header;
    header.magic = MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint32_t>(type);
    header.status_code = static_cast<uint32_t>(status_code);
    header.job_id_numeric = job_id_num;
    header.request_id = req_id;
    header.payload_len = static_cast<uint32_t>(payload.size());

    if (!write_exact(&header, sizeof(header))) {
        return false;
    }

    if (header.payload_len > 0) {
        if (!write_exact(payload.data(), payload.size())) {
            return false;
        }
    }

    return true;
}

bool IpcChannel::receive(MessageHeader& header_out, std::string& payload_out, int timeout_ms) {
    if (!read_exact(&header_out, sizeof(MessageHeader), timeout_ms)) {
        return false;
    }

    if (header_out.magic != MAGIC || header_out.version != PROTOCOL_VERSION) {
        return false;
    }

    if (header_out.payload_len > MAX_PAYLOAD_SIZE) {
        return false;
    }

    if (header_out.payload_len > 0) {
        payload_out.resize(header_out.payload_len);
        if (!read_exact(payload_out.data(), header_out.payload_len, timeout_ms)) {
            return false;
        }
    } else {
        payload_out.clear();
    }

    return true;
}

} // namespace astra
