#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <system_error>
#include <string_view>
#include <optional>

// ============================================================================
// RAII Classes
// ============================================================================

// Generic file descriptor wrapper with RAII
class FileDescriptor {
public:
    FileDescriptor() noexcept : fd_(-1) {}
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    // Non-copyable
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    // Movable
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~FileDescriptor() { reset(); }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // Release ownership (for async close via io_uring)
    [[nodiscard]] int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

// TCP Socket wrapper
class Socket : public FileDescriptor {
public:
    Socket() = default;
    explicit Socket(int fd) : FileDescriptor(fd) {}

    // Factory method to create a listening socket
    // Returns nullopt on failure (check errno for details)
    [[nodiscard]] static std::optional<Socket>
    create_listener(uint16_t port, int backlog = 128) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return std::nullopt;
        }

        Socket sock(fd);

        int opt = 1;
        if (::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            return std::nullopt;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            return std::nullopt;
        }

        if (::listen(sock.get(), backlog) < 0) {
            return std::nullopt;
        }

        return sock;
    }
};

// io_uring wrapper with RAII
class IoUring {
public:
    IoUring() : initialized_(false) {}

    // Non-copyable and non-movable
    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;
    IoUring(IoUring&&) = delete;
    IoUring& operator=(IoUring&&) = delete;

    ~IoUring() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
        }
    }

    [[nodiscard]] std::error_code init(unsigned entries, unsigned flags = 0) {
        int ret = io_uring_queue_init(entries, &ring_, flags);
        if (ret < 0) {
            return std::error_code(-ret, std::system_category());
        }
        initialized_ = true;
        return {};
    }

    [[nodiscard]] io_uring_sqe* get_sqe() {
        return io_uring_get_sqe(&ring_);
    }

    [[nodiscard]] int submit() {
        return io_uring_submit(&ring_);
    }

    [[nodiscard]] int wait_cqe(io_uring_cqe** cqe) {
        return io_uring_wait_cqe(&ring_, cqe);
    }

    void cqe_seen(io_uring_cqe* cqe) {
        io_uring_cqe_seen(&ring_, cqe);
    }

private:
    io_uring ring_;
    bool initialized_;
};

// ============================================================================
// Operation Types
// ============================================================================

enum class OpType : uint8_t {
    Accept,
    Send,
    Close
};

struct OpData {
    OpType type;
    int client_fd;
};

// ============================================================================
// io_uring Operations
// ============================================================================

constexpr std::string_view HELLO_MSG = "Hello, World!\n";

// Static OpData for Accept (reused)
static OpData accept_op_data{OpType::Accept, -1};

void submit_accept(IoUring& ring, int listen_fd) {
    io_uring_sqe* sqe = ring.get_sqe();
    if (!sqe) {
        std::cerr << "Failed to get SQE for accept\n";
        return;
    }

    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &accept_op_data);
    (void)ring.submit();
}

void submit_send(IoUring& ring, int client_fd) {
    io_uring_sqe* sqe = ring.get_sqe();
    if (!sqe) {
        std::cerr << "Failed to get SQE for send\n";
        ::close(client_fd);
        return;
    }

    io_uring_prep_send(sqe, client_fd, HELLO_MSG.data(), HELLO_MSG.size(), 0);

    auto* op_data = new OpData{OpType::Send, client_fd};
    io_uring_sqe_set_data(sqe, op_data);
    (void)ring.submit();
}

void submit_close(IoUring& ring, int client_fd) {
    io_uring_sqe* sqe = ring.get_sqe();
    if (!sqe) {
        std::cerr << "Failed to get SQE for close\n";
        ::close(client_fd);
        return;
    }

    io_uring_prep_close(sqe, client_fd);

    auto* op_data = new OpData{OpType::Close, client_fd};
    io_uring_sqe_set_data(sqe, op_data);
    (void)ring.submit();
}

void handle_completion(IoUring& ring, int listen_fd, io_uring_cqe* cqe) {
    auto* op_data = static_cast<OpData*>(io_uring_cqe_get_data(cqe));
    int result = cqe->res;

    switch (op_data->type) {
    case OpType::Accept: {
        if (result >= 0) {
            int client_fd = result;
            std::cout << "Accepted connection: fd=" << client_fd << "\n";
            submit_send(ring, client_fd);
        } else {
            std::cerr << "Accept error: " << strerror(-result) << "\n";
        }
        // Resubmit accept for next connection
        submit_accept(ring, listen_fd);
        break;
    }

    case OpType::Send: {
        if (result >= 0) {
            std::cout << "Sent " << result << " bytes to fd=" << op_data->client_fd << "\n";
        } else {
            std::cerr << "Send error: " << strerror(-result) << "\n";
        }
        submit_close(ring, op_data->client_fd);
        delete op_data;
        break;
    }

    case OpType::Close: {
        if (result < 0) {
            std::cerr << "Close error: " << strerror(-result) << "\n";
        } else {
            std::cout << "Closed connection: fd=" << op_data->client_fd << "\n";
        }
        delete op_data;
        break;
    }
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    constexpr uint16_t PORT = 8080;
    constexpr unsigned QUEUE_DEPTH = 256;

    std::cout << "Starting server on port " << PORT << "...\n";

    // Create listening socket
    auto listener_opt = Socket::create_listener(PORT);
    if (!listener_opt) {
        std::cerr << "Failed to create listener: " << strerror(errno) << "\n";
        return 1;
    }
    Socket listener = std::move(*listener_opt);
    std::cout << "Listening socket created: fd=" << listener.get() << "\n";

    // Initialize io_uring
    IoUring ring;
    if (auto ec = ring.init(QUEUE_DEPTH); ec) {
        std::cerr << "Failed to init io_uring: " << ec.message() << "\n";
        return 1;
    }
    std::cout << "io_uring initialized with queue depth " << QUEUE_DEPTH << "\n";

    // Submit initial accept
    submit_accept(ring, listener.get());

    std::cout << "Server running. Press Ctrl+C to stop.\n";

    // Event loop
    while (true) {
        io_uring_cqe* cqe;
        int ret = ring.wait_cqe(&cqe);
        if (ret < 0) {
            std::cerr << "wait_cqe error: " << strerror(-ret) << "\n";
            break;
        }

        handle_completion(ring, listener.get(), cqe);
        ring.cqe_seen(cqe);
    }

    return 0;
}
