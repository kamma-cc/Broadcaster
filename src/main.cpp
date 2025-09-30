#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

constexpr int QUEUE_DEPTH = 256;
constexpr int PORT = 8080;

enum class OpType {
    ACCEPT,
    CLOSE
};

struct RequestData {
    OpType type;
    int client_fd;
};

int setup_listening_socket(int port) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    int enable = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        perror("setsockopt");
        close(sock_fd);
        return -1;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

void add_accept_request(io_uring* ring, int server_fd, struct sockaddr_in* client_addr, socklen_t* client_len) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    auto* req = new RequestData{OpType::ACCEPT, 0};

    io_uring_prep_accept(sqe, server_fd, (struct sockaddr*)client_addr, client_len, 0);
    io_uring_sqe_set_data(sqe, req);
}

void handle_completion(io_uring* ring, io_uring_cqe* cqe, int server_fd,
                      struct sockaddr_in* client_addr, socklen_t* client_len) {
    auto* req = static_cast<RequestData*>(io_uring_cqe_get_data(cqe));

    if (req->type == OpType::ACCEPT) {
        int client_fd = cqe->res;
        if (client_fd >= 0) {
            std::cout << "Accepted new connection: fd=" << client_fd << std::endl;

            // Close the client immediately (we're just testing accept for now)
            close(client_fd);

            // Queue the next accept
            add_accept_request(ring, server_fd, client_addr, client_len);
        } else {
            std::cerr << "Accept failed: " << strerror(-client_fd) << std::endl;
        }
        delete req;
    }
}

int main() {
    std::cout << "Broadcaster starting on port " << PORT << "..." << std::endl;

    // Setup listening socket
    int server_fd = setup_listening_socket(PORT);
    if (server_fd < 0) {
        return 1;
    }
    std::cout << "Listening socket created: fd=" << server_fd << std::endl;

    // Initialize io_uring
    io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        close(server_fd);
        return 1;
    }
    std::cout << "io_uring initialized with queue depth " << QUEUE_DEPTH << std::endl;

    // Prepare for accept operations
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    // Submit initial accept request
    add_accept_request(&ring, server_fd, &client_addr, &client_len);
    io_uring_submit(&ring);

    std::cout << "Event loop starting... (Press Ctrl+C to stop)" << std::endl;

    // Event loop
    while (true) {
        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            std::cerr << "io_uring_wait_cqe: " << strerror(-ret) << std::endl;
            break;
        }

        handle_completion(&ring, cqe, server_fd, &client_addr, &client_len);
        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }

    // Cleanup
    io_uring_queue_exit(&ring);
    close(server_fd);

    return 0;
}