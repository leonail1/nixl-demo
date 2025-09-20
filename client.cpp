#include "nixl.h"
#include "util.h"
using namespace demo;

int main(int argc, char **argv)
{
	DemoOptions opts;
	int parseResult = handleParsing(argc, argv,
									/*requireRemoteIp=*/true,
									"nixl-demo-client", opts,
									makeUsagePrinter(argv[0], std::optional<std::string>{"--ip <server-ip>"}));
	if (parseResult >= 0)
	{
		return parseResult;
	}

	try
	{
		nixlAgentConfig cfg(/*use_prog_thread=*/true);
		nixlAgent agent(opts.agentName, cfg);

		nixl_b_params_t initParams;
		nixl_mem_list_t supportedMems;
		ensureSuccess(agent.getPluginParams("UCX", supportedMems, initParams), "getPluginParams(UCX)");

		nixlBackendH *backend = nullptr;
		ensureSuccess(agent.createBackend("UCX", initParams, backend), "createBackend(UCX)");

		nixl_opt_args_t optArgs;
		optArgs.backends.push_back(backend);
		optArgs.notifMsg = "nixl-demo-complete";
		optArgs.hasNotif = true;

		void *buffer = nullptr;
		if (posix_memalign(&buffer, 64, opts.bytes) != 0)
		{
			throw std::runtime_error("posix_memalign failed");
		}

		auto *bytes = static_cast<uint8_t *>(buffer);
		for (size_t i = 0; i < opts.bytes; ++i)
		{
			bytes[i] = static_cast<uint8_t>((i % 26) + 65);
		}

		nixl_reg_dlist_t regList(DRAM_SEG);
		nixlBlobDesc src;
		src.addr = reinterpret_cast<uintptr_t>(buffer);
		src.len = opts.bytes;
		src.devId = 0;
		regList.addDesc(src);
		ensureSuccess(agent.registerMem(regList, &optArgs), "registerMem");

		std::cout << "[client] Agent: " << opts.agentName
				  << ", UCX buffer @ 0x" << std::hex << src.addr << std::dec
				  << " size " << formatBytes(opts.bytes) << std::endl;

		const std::string &serverIp = *opts.remoteIp;

		std::cout << "[client] Requesting metadata from " << serverIp << ':' << opts.port << std::endl;
		std::string metadata = requestMetadata(serverIp, opts.port);

		std::string remoteAgent;
		ensureSuccess(agent.loadRemoteMD(metadata, remoteAgent), "loadRemoteMD");

		auto [parsedAgent, remotePool] = extractRemoteBuffer(metadata, DRAM_SEG);
		if (!remoteAgent.empty() && remoteAgent != parsedAgent)
		{
			std::cerr << "[client] Warning: remote agent name mismatch between metadata sources ("
					  << remoteAgent << " vs " << parsedAgent << ")" << std::endl;
		}

		nixl_xfer_dlist_t localXfer(DRAM_SEG);
		nixl_xfer_dlist_t remoteXfer(DRAM_SEG);

		uintptr_t localBase = reinterpret_cast<uintptr_t>(buffer);
		size_t remaining = opts.bytes;
		size_t localOffset = 0;

		for (int i = 0; i < remotePool.descCount() && remaining > 0; ++i)
		{
			nixlBasicDesc remoteDesc = remotePool[i];
			if (remoteDesc.len == 0)
			{
				continue;
			}
			size_t chunk = std::min(remoteDesc.len, remaining);

			nixlBasicDesc remoteChunk = remoteDesc;
			remoteChunk.len = chunk;
			remoteXfer.addDesc(remoteChunk);

			nixlBasicDesc localChunk;
			localChunk.addr = localBase + localOffset;
			localChunk.len = chunk;
			localChunk.devId = 0;
			localXfer.addDesc(localChunk);

			localOffset += chunk;
			remaining -= chunk;
		}

		if (remaining != 0)
		{
			throw std::runtime_error("remote metadata does not expose enough bytes for requested transfer");
		}

		std::cout << "[client] Prepared " << localXfer.descCount()
				  << " descriptors for transfer to agent '" << remoteAgent << "'" << std::endl;

		nixlXferReqH *handle = nullptr;
		ensureSuccess(agent.createXferReq(NIXL_READ, localXfer, remoteXfer, remoteAgent, handle, &optArgs),
					  "createXferReq");

		nixl_status_t status = agent.postXferReq(handle);
		while (status == NIXL_IN_PROG)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			status = agent.getXferStatus(handle);
		}
		ensureSuccess(status, "postXferReq");

		std::cout << "[client] Transfer completed" << std::endl;

		size_t nonZeroCount = 0;
		for (size_t i = 0; i < opts.bytes; ++i)
		{
			if (bytes[i] != 0)
			{
				++nonZeroCount;
			}
		}

		std::cout << "[server] Received " << nonZeroCount
				  << " non-zero bytes. First 16 bytes: ";
		size_t preview = std::min<size_t>(16, opts.bytes);
		for (size_t i = 0; i < preview; ++i)
		{
			std::cout << std::hex << std::setw(2) << std::setfill('0')
					  << static_cast<int>(bytes[i]) << (i + 1 == preview ? "" : " ");
		}
		std::cout << std::dec << std::setfill(' ') << std::endl;

		ensureSuccess(agent.releaseXferReq(handle), "releaseXferReq");
		ensureSuccess(agent.deregisterMem(regList, &optArgs), "deregisterMem");
		agent.invalidateRemoteMD(remoteAgent);

		std::free(buffer);

		return 0;
	}
	catch (const std::exception &ex)
	{
		std::cerr << "Client error: " << ex.what() << std::endl;
	}

	return 1;
}
