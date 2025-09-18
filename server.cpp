#include "nixl.h"
#include "util.h"
using namespace demo;

int main(int argc, char **argv) {
    DemoOptions opts;
    int parseResult = handleParsing(argc, argv,
                                     /*requireRemoteIp=*/false,
                                     "nixl-demo-server", opts,
                                     makeUsagePrinter(argv[0], std::nullopt));
    if (parseResult >= 0) {
        return parseResult;
    }

    try {
        nixlAgentConfig cfg(/*use_prog_thread=*/true,
                            /*use_listen_thread=*/true,
                            opts.port);
        nixlAgent agent(opts.agentName, cfg);

        nixl_b_params_t initParams;
        nixl_mem_list_t supportedMems;
        ensureSuccess(agent.getPluginParams("UCX", supportedMems, initParams), "getPluginParams(UCX)");

        nixlBackendH *backend = nullptr;
        ensureSuccess(agent.createBackend("UCX", initParams, backend), "createBackend(UCX)");

        nixl_opt_args_t optArgs;
        optArgs.backends.push_back(backend);

        void *buffer = nullptr;
        if (posix_memalign(&buffer, 64, opts.bytes) != 0) {
            throw std::runtime_error("posix_memalign failed");
        }
        std::memset(buffer, 0, opts.bytes);

        nixl_reg_dlist_t regList(DRAM_SEG);
        nixlBlobDesc region;
        region.addr = reinterpret_cast<uintptr_t>(buffer);
        region.len = opts.bytes;
        region.devId = 0;
        regList.addDesc(region);

        ensureSuccess(agent.registerMem(regList, &optArgs), "registerMem");

        std::cout << "[server] Agent: " << opts.agentName
                  << ", UCX buffer @ 0x" << std::hex << region.addr << std::dec
                  << " size " << formatBytes(opts.bytes)
                  << "\n[server] Listening for metadata on port " << opts.port << std::endl;

        std::cout << "[server] Waiting for UCX transfer..." << std::endl;

        nixl_notifs_t notifications;
        std::string remoteAgent;
        auto *bytes = static_cast<uint8_t *>(buffer);

        while (true) {
            nixl_status_t ret = agent.getNotifs(notifications, &optArgs);
            ensureSuccess(ret, "getNotifs");

            bool gotUpdate = false;
            for (auto &entry : notifications) {
                if (!entry.second.empty()) {
                    remoteAgent = entry.first;
                    std::cout << "[server] Notification from " << remoteAgent
                              << ": " << entry.second.front() << std::endl;
                    entry.second.clear();
                    gotUpdate = true;
                }
            }
            notifications.clear();

            if (gotUpdate) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        size_t nonZeroCount = 0;
        for (size_t i = 0; i < opts.bytes; ++i) {
            if (bytes[i] != 0) {
                ++nonZeroCount;
            }
        }

        std::cout << "[server] Received " << nonZeroCount
                  << " non-zero bytes. First 16 bytes: ";
        size_t preview = std::min<size_t>(16, opts.bytes);
        for (size_t i = 0; i < preview; ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(bytes[i]) << (i + 1 == preview ? "" : " ");
        }
        std::cout << std::dec << std::setfill(' ') << std::endl;

        ensureSuccess(agent.deregisterMem(regList, &optArgs), "deregisterMem");

        if (!remoteAgent.empty()) {
            agent.invalidateRemoteMD(remoteAgent);
        }

        std::free(buffer);

        std::cout << "[server] Cleanup complete." << std::endl;
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "Server error: " << ex.what() << std::endl;
    }

    return 1;
}
