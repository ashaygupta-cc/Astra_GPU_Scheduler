#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <utility>
#include <unistd.h>
#include <sys/socket.h>

namespace astra {

// RAII File Descriptor Wrapper
class UniqueFd {
public:
    UniqueFd() noexcept : fd_(-1) {}
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    int release() noexcept {
        int old = fd_;
        fd_ = -1;
        return old;
    }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_ = -1;
};

enum class MessageType : uint32_t {
    UNKNOWN        = 0,
    RUN_JOB        = 1,
    JOB_STARTED    = 2,
    JOB_PROGRESS   = 3,
    JOB_COMPLETED  = 4,
    JOB_FAILED     = 5,
    CANCEL_JOB     = 6,
    HEARTBEAT      = 7,
    SHUTDOWN       = 8,
    ERROR          = 9
};

inline constexpr std::string_view to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::UNKNOWN:       return "UNKNOWN";
        case MessageType::RUN_JOB:       return "RUN_JOB";
        case MessageType::JOB_STARTED:   return "JOB_STARTED";
        case MessageType::JOB_PROGRESS:  return "JOB_PROGRESS";
        case MessageType::JOB_COMPLETED: return "JOB_COMPLETED";
        case MessageType::JOB_FAILED:    return "JOB_FAILED";
        case MessageType::CANCEL_JOB:    return "CANCEL_JOB";
        case MessageType::HEARTBEAT:     return "HEARTBEAT";
        case MessageType::SHUTDOWN:      return "SHUTDOWN";
        case MessageType::ERROR:         return "ERROR";
    }
    return "UNKNOWN";
}

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = 0x41535452; // 'ASTR'
    uint32_t version = 1;
    uint32_t type = 0;            // static_cast<uint32_t>(MessageType)
    uint32_t status_code = 0;     // 0 = OK, non-zero = error
    uint64_t job_id_numeric = 0;
    uint32_t request_id = 0;
    uint32_t payload_len = 0;
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 32, "MessageHeader must be exactly 32 bytes");

class IpcChannel {
public:
    static constexpr uint32_t MAGIC = 0x41535452;
    static constexpr uint32_t PROTOCOL_VERSION = 1;
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 64 * 1024; // 64 KB bounded payload

    explicit IpcChannel(UniqueFd fd);
    ~IpcChannel() = default;

    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;
    IpcChannel(IpcChannel&&) noexcept = default;
    IpcChannel& operator=(IpcChannel&&) noexcept = default;

    int fd() const noexcept { return fd_.get(); }
    bool is_open() const noexcept { return fd_.valid(); }
    void close() noexcept { fd_.reset(); }

    bool send(MessageType type, uint64_t job_id_num, uint32_t req_id, 
              int32_t status_code, const std::string& payload);

    bool receive(MessageHeader& header_out, std::string& payload_out, int timeout_ms = -1);

    static std::pair<UniqueFd, UniqueFd> create_socketpair();

private:
    UniqueFd fd_;

    bool write_exact(const void* buf, size_t count);
    bool read_exact(void* buf, size_t count, int timeout_ms);
};

} // namespace astra
