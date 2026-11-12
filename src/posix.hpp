#ifndef ORCLI_POSIX_HPP
#define ORCLI_POSIX_HPP

// Small owners and error helpers for POSIX descriptors, shared by the code that runs child
// processes (systemd-creds and group members).

#include "common.hpp"

#include <signal.h>

#include <array>
#include <cerrno>
#include <expected>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace orcli::posix {

inline std::unexpected<Error> sys_fail_code(std::string_view action, int error_number) {
    return fail(Exit::config,
                std::format("cannot {}: {}", action,
                            std::error_code(error_number, std::system_category()).message()));
}

// Reads errno first, before any other call can overwrite it.
inline std::unexpected<Error> sys_fail(std::string_view action) {
    return sys_fail_code(action, errno);
}

// SIGINT, SIGTERM and SIGHUP: the signals that stop a question. Built through a struct
// sigaction member because sigset_t's own header is private to glibc.
inline auto stop_signals() {
    struct sigaction holder{};
    sigemptyset(&holder.sa_mask);
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        sigaddset(&holder.sa_mask, signal_number);
    }
    return holder.sa_mask;
}

template <class Call> auto retry_eintr(Call call) {
    for (;;) {
        const auto value = call();
        if (value != -1 || errno != EINTR) {
            return value;
        }
    }
}

class Descriptor {
public:
    explicit Descriptor(int value = -1) noexcept : value_(value) {}
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

    // close(2) is not retried on EINTR: on Linux the descriptor is released either way.
    void reset() noexcept {
        if (value_ >= 0) {
            ::close(std::exchange(value_, -1));
        }
    }

private:
    int value_;
};

struct Pipe {
    Descriptor read_end;
    Descriptor write_end;
};

// O_CLOEXEC keeps every pipe end out of a child except the ones placed on its standard streams.
inline Result<Pipe> make_pipe(std::string_view purpose) {
    std::array<int, 2> ends{};
    if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
        return sys_fail(std::format("create a pipe for {}", purpose));
    }
    return Pipe{.read_end = Descriptor(ends.at(0)), .write_end = Descriptor(ends.at(1))};
}

} // namespace orcli::posix

#endif // ORCLI_POSIX_HPP
