#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>
#include "nixl.h"
#include "serdes/serdes.h"

namespace demo {

struct DemoOptions {
    size_t bytes = 1 << 20;
    int port = default_comm_port;
    std::string agentName;
    std::optional<std::string> remoteIp;
};

inline void ensureSuccess(nixl_status_t status, const std::string &what) {
    if (status != NIXL_SUCCESS) {
        std::ostringstream oss;
        oss << what << " failed with status " << status;
        throw std::runtime_error(oss.str());
    }
}

inline std::string formatBytes(size_t bytes) {
    const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    size_t idx = 0;
    while (idx + 1 < std::size(units) && value >= 1024.0) {
        value /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << value << ' ' << units[idx];
    return oss.str();
}

inline bool argumentIs(int argc, char **argv, int index, std::string_view flag) {
    return index < argc && flag == argv[index];
}

inline std::string requireValue(int argc, char **argv, int &index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for argument " + std::string(argv[index]));
    }
    return argv[++index];
}

inline DemoOptions parseArgs(int argc,
                             char **argv,
                             bool requireRemoteIp,
                             const std::string &defaultAgent) {
    DemoOptions opts;
    opts.agentName = defaultAgent;

    for (int i = 1; i < argc; ++i) {
        if (argumentIs(argc, argv, i, "--size")) {
            std::string raw = requireValue(argc, argv, i);
            try {
                opts.bytes = std::stoull(raw, nullptr, 0);
            } catch (const std::exception &ex) {
                throw std::invalid_argument("invalid size value '" + raw + "': " + ex.what());
            }
        } else if (argumentIs(argc, argv, i, "--ip")) {
            opts.remoteIp = requireValue(argc, argv, i);
        } else if (argumentIs(argc, argv, i, "--port")) {
            opts.port = std::stoi(requireValue(argc, argv, i));
        } else if (argumentIs(argc, argv, i, "--agent")) {
            opts.agentName = requireValue(argc, argv, i);
        } else if (argumentIs(argc, argv, i, "--help")) {
            throw std::invalid_argument("");
        } else {
            std::ostringstream oss;
            oss << "unknown argument: " << argv[i];
            throw std::invalid_argument(oss.str());
        }
    }

    if (requireRemoteIp && !opts.remoteIp) {
        throw std::invalid_argument("--ip is required");
    }
    if (opts.port <= 0 || opts.port > 65535) {
        throw std::invalid_argument("port must be in range 1-65535");
    }
    if (opts.bytes == 0) {
        throw std::invalid_argument("size must be greater than zero");
    }

    return opts;
}

inline int handleParsing(int argc,
                         char **argv,
                         bool requireRemoteIp,
                         const std::string &defaultAgent,
                         DemoOptions &outOpts,
                         const std::function<void()> &usagePrinter) {
    try {
        outOpts = parseArgs(argc, argv, requireRemoteIp, defaultAgent);
        return -1;
    } catch (const std::invalid_argument &ex) {
        if (std::strlen(ex.what()) == 0) {
            usagePrinter();
            return 0;
        }
        std::cerr << "Error: " << ex.what() << std::endl;
        usagePrinter();
        return 1;
    } catch (const std::exception &ex) {
        std::cerr << "Unexpected error: " << ex.what() << std::endl;
        return 1;
    }
}

inline auto makeUsagePrinter(const char *prog,
                             const std::optional<std::string> &extra) {
    return [prog, extra]() {
        std::cout << "Usage: " << prog
                  << " [--size <bytes>] [--port <port>] [--agent <name>]";
        if (extra) {
            std::cout << ' ' << *extra;
        }
        std::cout << std::endl;
    };
}

inline int openSocket(const std::string &ip, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("failed to create socket: " + std::string(std::strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::invalid_argument("invalid IPv4 address: " + ip);
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("connect(" + ip + ":" + std::to_string(port) + ") failed: " + err);
    }

    return fd;
}

inline void sendSizedMessage(int fd, const std::string &payload) {
    size_t size = payload.size();
    std::vector<char> buffer(sizeof(size) + payload.size());
    std::memcpy(buffer.data(), &size, sizeof(size));
    if (!payload.empty()) {
        std::memcpy(buffer.data() + sizeof(size), payload.data(), payload.size());
    }

    size_t sent = 0;
    while (sent < buffer.size()) {
        ssize_t rc = ::send(fd, buffer.data() + sent, buffer.size() - sent, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("send failed: " + std::string(std::strerror(errno)));
        }
        sent += static_cast<size_t>(rc);
    }
}

inline std::string recvSizedMessage(int fd) {
    size_t size = 0;
    size_t received = 0;
    while (received < sizeof(size)) {
        ssize_t rc = ::recv(fd,
                            reinterpret_cast<char*>(&size) + received,
                            sizeof(size) - received,
                            0);
        if (rc <= 0) {
            if (rc == 0 || errno == ECONNRESET) {
                throw std::runtime_error("connection closed while waiting for message header");
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("recv failed: " + std::string(std::strerror(errno)));
        }
        received += static_cast<size_t>(rc);
    }

    std::string payload(size, '\0');
    received = 0;
    while (received < size) {
        ssize_t rc = ::recv(fd, payload.data() + received, size - received, 0);
        if (rc <= 0) {
            if (rc == 0 || errno == ECONNRESET) {
                throw std::runtime_error("connection closed while waiting for payload");
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("recv failed: " + std::string(std::strerror(errno)));
        }
        received += static_cast<size_t>(rc);
    }

    return payload;
}

inline std::string requestMetadata(const std::string &ip, int port) {
    int fd = openSocket(ip, port);
    try {
        sendSizedMessage(fd, "NIXLCOMM:SEND");
        std::string response = recvSizedMessage(fd);
        ::close(fd);

        constexpr std::string_view prefix = "NIXLCOMM:LOAD";
        if (response.rfind(prefix, 0) != 0) {
            throw std::runtime_error("unexpected metadata response: " + response);
        }
        return response.substr(prefix.size());
    } catch (...) {
        ::close(fd);
        throw;
    }
}

inline std::pair<std::string, nixl_xfer_dlist_t>
extractRemoteBuffer(const std::string &metadata, nixl_mem_t desiredMem = DRAM_SEG) {
    nixlSerDes sd;
    ensureSuccess(sd.importStr(metadata), "import metadata");

    std::string agent = sd.getStr("Agent");

    size_t connCount = 0;
    ensureSuccess(sd.getBuf("Conns", &connCount, sizeof(connCount)), "read connection count");
    for (size_t i = 0; i < connCount; ++i) {
        (void)sd.getStr("t");
        (void)sd.getStr("c");
    }

    std::string marker = sd.getStr("");
    if (marker != "MemSection") {
        throw std::runtime_error("unexpected metadata marker");
    }

    size_t segmentCount = 0;
    ensureSuccess(sd.getBuf("nixlSecElms", &segmentCount, sizeof(segmentCount)), "read segment count");

    for (size_t i = 0; i < segmentCount; ++i) {
        std::string backend = sd.getStr("bknd");
        nixl_reg_dlist_t regList(&sd);
        if (regList.descCount() == 0) {
            continue;
        }
        if (regList.getType() != desiredMem) {
            continue;
        }

        nixl_xfer_dlist_t xfer = regList.trim();
        return {agent, xfer};
    }

    throw std::runtime_error("remote metadata does not expose desired memory segment");
}

} // namespace demo
