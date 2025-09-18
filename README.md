# NIXL UCX Demo

This demo showcases a minimal two-process data transfer using the NIXL UCX backend.  
`nixl_demo_server` advertises a DRAM buffer over the local metadata listener, while `nixl_demo_client` retrieves the metadata through a lightweight socket helper, registers its own buffer, and performs a UCX write into the server buffer.  The example focuses on CPU memory paths and keeps the control plane explicit so you can adapt it to your own environment.

## Prerequisites

- NIXL installed locally (default path: `/opt/nvidia/nvda_nixl`).
- C++17 compiler toolchain.
- CMake 3.16 or newer.
- UCX libraries shipped with your NIXL installation (no additional packages needed).

If NIXL is installed somewhere else, set `-DNIXL_ROOT=<path>` during configuration so the build can locate headers and libraries.

## Build Instructions

```bash
cd demo
cmake -S . -B build \
      -DNIXL_ROOT=/opt/nvidia/nvda_nixl   # optional if NIXL is elsewhere
cmake --build build
```

Both `nixl_demo_server` and `nixl_demo_client` will be generated in `demo/build/` (and can be installed via `cmake --install build`).

## Runtime Overview

1. **Start the server** on the host that exposes the UCX-accessible buffer.  It enables the NIXL listener thread and waits for a notification from the client.
   ```bash
   ./build/nixl_demo_server --size 1048576 --port 12345 --agent server-agent
   ```
   - `--size` is the buffer length in bytes (default: 1 MiB).
   - `--port` overrides the metadata listener port (default: 8888).
   - `--agent` assigns the local NIXL agent name (default: `nixl-demo-server`).

2. **Launch the client** from another terminal or machine, pointing it to the server's IP/port.  The client requests the server metadata, prepares matching descriptors, and posts a UCX write carrying the pattern-filled buffer.
   ```bash
   ./build/nixl_demo_client --ip 192.168.10.131 --size 1048576 --port 12345 --agent client-agent
   ```
   - `--ip` is required; it must reach the server's listener.
   - `--size` must not exceed what the server advertised.
   - `--agent` sets the client NIXL agent name (default: `nixl-demo-client`).

3. The server prints a notification once the transfer completes and shows a hexdump of the first bytes written by the client.  Both executables clean up registrations and invalidate metadata before exiting.

   ```bash
   $ ./build/nixl_demo_server --size 1048576 --port 12345 --agent server-agent
   [server] Agent: server-agent, UCX buffer @ 0x153d953de040 size 1.00 MiB
   [server] Listening for metadata on port 12345
   [server] Waiting for UCX transfer...
   [server] Notification from client-agent: nixl-demo-complete
   [server] Received 1048576 non-zero bytes. First 16 bytes: 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f 50
   [server] Cleanup complete.
   ```

   ```bash
   $ ./build/nixl_demo_client --ip 192.168.10.131 --size 1048576 --port 12345 --agent client-agent
   [client] Agent: client-agent, UCX buffer @ 0x1490a45b0040 size 1.00 MiB
   [client] Requesting metadata from 192.168.10.131:12345
   [client] Prepared 1 descriptors for transfer to agent 'server-agent'
   [client] Transfer completed
   ```   


## How Metadata Exchange Works

- The server publishes its descriptors via the NIXL listener thread.
- `requestMetadata()` in `util.h` opens a short-lived TCP connection to the server, issues the `NIXLCOMM:SEND` command, and retrieves the serialized metadata blob.
- The client feeds that blob to `loadRemoteMD()` and mirrors the descriptors into a transfer request, keeping the control path simple and explicit.  You can replace the helper with ETCD or another control plane without touching the UCX transfer logic.
