#include "nixl.h"
#include "util.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <cassert>
using namespace demo;

/**
 * @brief 读取3FS文件块
 *
 * @param agent NIXL代理
 * @param fd 文件描述符
 * @param offset 文件偏移量
 * @param size 读取大小
 * @return void* 成功返回读取到的内存指针，失败返回nullptr
 */
void *
read_3fs_block(const std::string &agent_name,
			   nixlAgent &agent,
			   int fd,
			   size_t offset,
			   size_t size)
{
	/* 申请 4K 对齐内存 */
	void *buf = nullptr;
	if (posix_memalign(&buf, 4096, size) != 0)
	{
		std::cerr << "memalign failed\n";
		return nullptr;
	}

	/* 注册 DRAM 段 */
	nixl_reg_dlist_t dram_reg(DRAM_SEG);
	nixlBlobDesc dram_desc{reinterpret_cast<uintptr_t>(buf), size, 0};
	dram_reg.addDesc(dram_desc);
	if (agent.registerMem(dram_reg) != NIXL_SUCCESS)
	{
		std::cerr << "Failed to register DRAM memory\n";
		free(buf);
		return nullptr;
	}

	/* 注册文件段（单块） */
	nixl_reg_dlist_t file_reg(FILE_SEG);
	nixlBlobDesc file_desc{offset, size, static_cast<uint64_t>(fd)};
	file_reg.addDesc(file_desc);
	if (agent.registerMem(file_reg) != NIXL_SUCCESS)
	{
		std::cerr << "Failed to register file segment\n";
		agent.deregisterMem(dram_reg);
		free(buf);
		return nullptr;
	}

	/* 构造传输列表 */
	nixl_xfer_dlist_t src_list(FILE_SEG), dst_list(DRAM_SEG);
	src_list.addDesc(file_desc);
	dst_list.addDesc(dram_desc);

	/* 创建并发起 READ 请求 */
	nixlXferReqH *req = nullptr;
	if (agent.createXferReq(NIXL_READ, src_list, dst_list, agent_name, req) != NIXL_SUCCESS)
	{
		std::cerr << "Failed to create transfer request\n";
		agent.deregisterMem(file_reg);
		agent.deregisterMem(dram_reg);
		free(buf);
		return nullptr;
	}

	if (agent.postXferReq(req) < 0)
	{
		std::cerr << "Failed to post transfer request\n";
		agent.releaseXferReq(req);
		agent.deregisterMem(file_reg);
		agent.deregisterMem(dram_reg);
		free(buf);
		return nullptr;
	}

	/* 等待完成 */
	nixl_status_t st;
	do
	{
		st = agent.getXferStatus(req);
	} while (st == NIXL_IN_PROG);

	if (st != NIXL_SUCCESS)
	{
		std::cerr << "Transfer failed with status: " << st << '\n';
		agent.releaseXferReq(req);
		agent.deregisterMem(file_reg);
		agent.deregisterMem(dram_reg);
		free(buf);
		return nullptr;
	}

	/* 清理资源 */
	agent.releaseXferReq(req);
	agent.deregisterMem(file_reg);
	agent.deregisterMem(dram_reg);

	return buf; // 成功时返回读取到的内存指针
}

int main(int argc, char **argv)
{
	DemoOptions opts;
	int parseResult = handleParsing(argc, argv,
									/*requireRemoteIp=*/false,
									"nixl-demo-server", opts,
									makeUsagePrinter(argv[0], std::nullopt));
	if (parseResult >= 0)
	{
		return parseResult;
	}

	try
	{
		const char *file_path = "/3fs/stage/file.bin";

		/* 1. 打开 3fs 文件并取大小 */
		int fd = open(file_path, O_RDONLY | O_DIRECT);
		if (fd < 0)
		{
			perror("open");
			return 1;
		}
		struct stat st{};
		if (fstat(fd, &st) < 0)
		{
			perror("fstat");
			return 1;
		}
		const size_t total_bytes = st.st_size;

		/* 2. 初始化 NIXL + HF3FS 后端 */
		std::string hf3fs_agent_name = "HF3FSReader";
		nixlAgent hf3fs_agent(hf3fs_agent_name, nixlAgentConfig(true));
		nixlBackendH *be = nullptr;
		assert(hf3fs_agent.createBackend("HF3FS", nixl_b_params_t{}, be) == NIXL_SUCCESS);

		void *buffer = read_3fs_block(hf3fs_agent_name, hf3fs_agent, fd, 0, total_bytes);
		std::cout<< "Read From Hf3fs Succeed with data: ";
		unsigned char *ptr = static_cast<unsigned char *>(buffer);
		for (int i = 0; i < 16; ++i)
		{
			std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
					  << static_cast<int>(ptr[i]) << " ";
		}
		std::cout << std::dec << std::endl; // 恢复十进制输出（可选）

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

		nixl_reg_dlist_t regList(DRAM_SEG);
		nixlBlobDesc region;
		region.addr = reinterpret_cast<uintptr_t>(buffer);
		region.len = total_bytes;
		region.devId = 0;
		regList.addDesc(region);

		ensureSuccess(agent.registerMem(regList, &optArgs), "registerMem");

		std::cout << "[server] Agent: " << opts.agentName
				  << ", UCX buffer @ 0x" << std::hex << region.addr << std::dec
				  << " size " << formatBytes(total_bytes)
				  << "\n[server] Listening for metadata on port " << opts.port << std::endl;

		std::cout << "[server] Waiting for UCX transfer..." << std::endl;

		nixl_notifs_t notifications;
		std::string remoteAgent;
		auto *bytes = static_cast<uint8_t *>(buffer);

		while (true)
		{
			nixl_status_t ret = agent.getNotifs(notifications, &optArgs);
			ensureSuccess(ret, "getNotifs");

			bool gotUpdate = false;
			for (auto &entry : notifications)
			{
				if (!entry.second.empty())
				{
					remoteAgent = entry.first;
					std::cout << "[server] Notification from " << remoteAgent
							  << ": " << entry.second.front() << std::endl;
					entry.second.clear();
					gotUpdate = true;
				}
			}
			notifications.clear();

			if (gotUpdate)
			{
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		ensureSuccess(agent.deregisterMem(regList, &optArgs), "deregisterMem");

		if (!remoteAgent.empty())
		{
			agent.invalidateRemoteMD(remoteAgent);
		}

		std::free(buffer);

		std::cout << "[server] Cleanup complete." << std::endl;
		return 0;
	}
	catch (const std::exception &ex)
	{
		std::cerr << "Server error: " << ex.what() << std::endl;
	}

	return 1;
}
