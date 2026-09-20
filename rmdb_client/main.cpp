#include <netdb.h>
#include <netinet/in.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define PORT_DEFAULT 8765

namespace wire_client {

constexpr uint32_t kMaxPayload = 1U << 20;
constexpr uint8_t kHandshake[8] = {'R', 'M', 'D', 'B', 0, 3, 0, 0};

struct Frame {
    uint8_t tag;
    std::vector<uint8_t> payload;
};

struct Column {
    std::string name;
    uint8_t type;
};

void read_exact(int fd, void* destination, size_t length) {
    auto* data = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < length) {
        ssize_t count = read(fd, data + done, length - done);
        if (count > 0) {
            done += static_cast<size_t>(count);
        } else if (count == 0) {
            throw std::runtime_error("connection closed during response");
        } else if (errno != EINTR) {
            throw std::runtime_error(std::string("socket read failed: ") + strerror(errno));
        }
    }
}

void write_all(int fd, const void* source, size_t length) {
    const auto* data = static_cast<const uint8_t*>(source);
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, data + done, length - done);
        if (count > 0) {
            done += static_cast<size_t>(count);
        } else if (count == 0) {
            throw std::runtime_error("socket write made no progress");
        } else if (errno != EINTR) {
            throw std::runtime_error(std::string("socket write failed: ") + strerror(errno));
        }
    }
}

uint16_t take_u16(const std::vector<uint8_t>& payload, size_t& offset) {
    if (offset + 2 > payload.size()) throw std::runtime_error("truncated u16");
    uint16_t value = (static_cast<uint16_t>(payload[offset]) << 8) |
                     payload[offset + 1];
    offset += 2;
    return value;
}

uint32_t take_u32(const std::vector<uint8_t>& payload, size_t& offset) {
    if (offset + 4 > payload.size()) throw std::runtime_error("truncated u32");
    uint32_t value = (static_cast<uint32_t>(payload[offset]) << 24) |
                     (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                     (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                     payload[offset + 3];
    offset += 4;
    return value;
}

Frame read_frame(int fd) {
    uint8_t header[8];
    read_exact(fd, header, sizeof(header));
    uint32_t size = (static_cast<uint32_t>(header[0]) << 24) |
                    (static_cast<uint32_t>(header[1]) << 16) |
                    (static_cast<uint32_t>(header[2]) << 8) | header[3];
    if (size > kMaxPayload || header[5] != 0 || header[6] != 0 || header[7] != 0) {
        throw std::runtime_error("invalid response frame header");
    }
    Frame frame{header[4], std::vector<uint8_t>(size)};
    if (size != 0) read_exact(fd, frame.payload.data(), size);
    return frame;
}

void write_exec_stream(int fd, const std::string& sql) {
    if (sql.empty() || sql.size() > kMaxPayload || sql.find('\0') != std::string::npos) {
        throw std::runtime_error("SQL length is outside EXEC_STREAM contract");
    }
    uint32_t size = static_cast<uint32_t>(sql.size());
    uint8_t header[8] = {static_cast<uint8_t>(size >> 24),
                         static_cast<uint8_t>(size >> 16),
                         static_cast<uint8_t>(size >> 8),
                         static_cast<uint8_t>(size), 0x20, 0, 0, 0};
    write_all(fd, header, sizeof(header));
    write_all(fd, sql.data(), sql.size());
}

std::vector<Column> decode_meta(const std::vector<uint8_t>& payload) {
    size_t offset = 0;
    uint16_t count = take_u16(payload, offset);
    if (count == 0) throw std::runtime_error("META has no columns");
    std::vector<Column> columns;
    columns.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t name_size = take_u16(payload, offset);
        if (name_size == 0 || offset + name_size + 1 > payload.size()) {
            throw std::runtime_error("invalid META column");
        }
        std::string name(reinterpret_cast<const char*>(payload.data() + offset),
                         name_size);
        offset += name_size;
        uint8_t type = payload[offset++];
        if (type < 1 || type > 3) throw std::runtime_error("unknown SQL type");
        columns.push_back({std::move(name), type});
    }
    if (offset != payload.size()) throw std::runtime_error("META has trailing bytes");
    return columns;
}

void print_row(const std::vector<uint8_t>& payload,
               const std::vector<Column>& columns) {
    size_t offset = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (offset >= payload.size()) throw std::runtime_error("truncated ROW");
        uint8_t present = payload[offset++];
        if (i != 0) std::cout << " | ";
        if (present == 0) {
            std::cout << "NULL";
            continue;
        }
        if (present != 1) throw std::runtime_error("invalid ROW present byte");
        if (columns[i].type == 1) {
            int32_t value = static_cast<int32_t>(take_u32(payload, offset));
            std::cout << value;
        } else if (columns[i].type == 2) {
            uint32_t bits = take_u32(payload, offset);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            std::cout << value;
        } else {
            uint32_t size = take_u32(payload, offset);
            if (offset + size > payload.size()) throw std::runtime_error("truncated CHAR");
            std::cout.write(reinterpret_cast<const char*>(payload.data() + offset), size);
            offset += size;
        }
    }
    if (offset != payload.size()) throw std::runtime_error("ROW has trailing bytes");
    std::cout << '\n';
}

void read_response(int fd) {
    std::vector<Column> columns;
    uint64_t rows = 0;
    bool saw_meta = false;
    while (true) {
        Frame frame = read_frame(fd);
        if (frame.tag == 0x01) {
            if (saw_meta) throw std::runtime_error("duplicate META");
            columns = decode_meta(frame.payload);
            saw_meta = true;
            for (size_t i = 0; i < columns.size(); ++i) {
                if (i != 0) std::cout << " | ";
                std::cout << columns[i].name;
            }
            std::cout << '\n';
        } else if (frame.tag == 0x02) {
            if (!saw_meta) throw std::runtime_error("ROW before META");
            print_row(frame.payload, columns);
            ++rows;
        } else if (frame.tag == 0x10) {
            if (!frame.payload.empty() || saw_meta) throw std::runtime_error("invalid COMMAND_OK");
            std::cout << "OK\n";
            return;
        } else if (frame.tag == 0x11) {
            if (!saw_meta || frame.payload.size() != 8) throw std::runtime_error("invalid RESULT_END");
            uint64_t declared = 0;
            for (uint8_t byte : frame.payload) declared = (declared << 8) | byte;
            if (declared != rows) throw std::runtime_error("RESULT_END row count mismatch");
            std::cout << "(" << rows << " row(s))\n";
            return;
        } else if (frame.tag == 0x12 || frame.tag == 0x13) {
            std::string message(frame.payload.begin(), frame.payload.end());
            std::cerr << (frame.tag == 0x12 ? "ABORT: " : "ERROR: ") << message << '\n';
            return;
        } else {
            throw std::runtime_error("unknown response frame tag");
        }
    }
}

void handshake(int fd) {
    write_all(fd, kHandshake, sizeof(kHandshake));
    uint8_t echoed[sizeof(kHandshake)];
    read_exact(fd, echoed, sizeof(echoed));
    if (std::memcmp(echoed, kHandshake, sizeof(echoed)) != 0) {
        throw std::runtime_error("server rejected RMDB Wire Protocol v3");
    }
}

}  // namespace wire_client

bool is_exit_command(std::string &cmd) { return cmd == "exit" || cmd == "exit;" || cmd == "bye" || cmd == "bye;"; }

int init_unix_sock(const char *unix_sock_path) {
    int sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "failed to create unix socket. %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sun_family = PF_UNIX;
    snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", unix_sock_path);

    if (connect(sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
        fprintf(stderr, "failed to connect to server. unix socket path '%s'. error %s", sockaddr.sun_path,
                strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int init_tcp_sock(const char *server_host, int server_port) {
    struct hostent *host;
    struct sockaddr_in serv_addr;

    if ((host = gethostbyname(server_host)) == NULL) {
        fprintf(stderr, "gethostbyname failed. errmsg=%d:%s\n", errno, strerror(errno));
        return -1;
    }

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "create socket error. errmsg=%d:%s\n", errno, strerror(errno));
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    serv_addr.sin_addr = *((struct in_addr *)host->h_addr);
    bzero(&(serv_addr.sin_zero), 8);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) == -1) {
        fprintf(stderr, "Failed to connect. errmsg=%d:%s\n", errno, strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int main(int argc, char *argv[]) {
    int ret = 0;  // set_terminal_noncanonical();
                  //    if (ret < 0) {
                  //        printf("Warning: failed to set terminal non canonical. Long command may be "
                  //               "handled incorrect\n");
                  //    }

    const char *unix_socket_path = nullptr;
    const char *server_host = "127.0.0.1";
    int server_port = PORT_DEFAULT;
    int opt;

    while ((opt = getopt(argc, argv, "s:h:p:")) > 0) {
        switch (opt) {
            case 's':
                unix_socket_path = optarg;
                break;
            case 'p':
                char *ptr;
                server_port = (int)strtol(optarg, &ptr, 10);
                break;
            case 'h':
                server_host = optarg;
                break;
            default:
                break;
        }
    }

    // const char *prompt_str = "RucBase > ";

    int sockfd;

    if (unix_socket_path != nullptr) {
        sockfd = init_unix_sock(unix_socket_path);
    } else {
        sockfd = init_tcp_sock(server_host, server_port);
    }
    if (sockfd < 0) {
        return 1;
    }

    try {
        wire_client::handshake(sockfd);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        close(sockfd);
        return 1;
    }

    while (1) {
        char *line_read = readline("Rucbase> ");
        if (line_read == nullptr) {
            // EOF encountered
            break;
        }
        std::string command = line_read;
        free(line_read);

        if (!command.empty()) {
            add_history(command.c_str());
            if (is_exit_command(command)) {
                printf("The client will be closed.\n");
                break;
            }

            try {
                wire_client::write_exec_stream(sockfd, command);
                wire_client::read_response(sockfd);
            } catch (const std::exception& error) {
                std::cerr << "Connection was broken: " << error.what() << '\n';
                break;
            }
        }
    }
    close(sockfd);
    printf("Bye.\n");
    return 0;
}
