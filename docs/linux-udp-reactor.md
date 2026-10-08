# Optional Linux UDP reactor and shared application payloads

The optional UDP reactor preserves GNS connection IDs, packet format, session
encryption, congestion control, lane semantics and the public interface ABI.
`USE_IO_URING` defaults to OFF.

## Build and validation

Requires Linux and liburing >= 2.4:

```sh
cmake -S . -B build-uring -DCMAKE_BUILD_TYPE=Release \
  -DUSE_IO_URING=ON -DBUILD_TESTS=ON -DENABLE_ICE=OFF
cmake --build build-uring -j8
ctest --test-dir build-uring \
  -R 'connection_quick|crypto|shared_payload|iouring_udp|service_fairness' \
  --output-on-failure
```

Configure a separate build with `USE_IO_URING=OFF` to test the classic socket
backend. The installed CMake package propagates liburing for static consumers.
The low-level io_uring check returns CTest's skip code 77 when the kernel or
sandbox cannot provide the required facility; skips are not performance evidence.

## Reactor

The existing service thread owns submission/completion rings, using
`IORING_SETUP_SINGLE_ISSUER`. A 1,024-entry submission ring and 8,192-entry
completion ring serve all raw sockets. `recvmsg` multishot uses 2,048 supplied
4 KiB buffers, validates address/control lengths and truncation, and invokes the
same existing raw UDP dispatch path. Impairment, packet tracing and ECN/TOS
handling retain their existing semantics.

Supplied buffers eliminate repeated receive submissions. **This is not NIC
zero-copy receive.** Hardware ZCRX needs a separate capability path, compatible
protocol support and configured RX queues/header splitting/steering; loopback
cannot validate it. The hardware requirements are described in the
[Linux ZCRX documentation](https://cdn.kernel.org/doc/html/latest/networking/iou-zcrx.html).

Vectored sendmsg submissions own their bytes and vector descriptors until their
send completions. GNS packet builders use temporary stack/crypto buffers, so
borrowing those vectors asynchronously would be unsafe. Stable, lazily allocated
send slots provide a bounded pool of 4,096 datagrams, with inline 2 KiB/8-vector
storage and larger-storage fallbacks. A full pool returns false to the existing
synchronous send path. This is asynchronous vectored dispatch, not a claim of
zero-copy encryption or transmission.

Only the service thread pumps the ring. QueueSend/Detach can run under the same
library serialization from other threads. Receiver identities outlive numeric
file descriptors. Detach suppresses old callbacks, cancels outstanding receives,
and keeps templates/buffers alive until completion. Shutdown cancels/drains before
releasing registered storage. Unsupported runtime operations fall back to the
classic socket path; manual polling also retains that path.

CQ dispatch yields after 512 completions and re-wakes when work remains. The
socket loop and due-thinker loop each have a 2 ms cooperative budget, preventing
continuous RX or many send-ready peers from starving other service work and
application API calls. A budget cannot interrupt an individual callback.

Tests cover multishot delivery, overwrite-after-enqueue, large vectors/bytes,
4,096-slot overflow/completion reuse, descriptor reuse before cancellation,
queued-send retirement, reinitialization, and cooperative thinker fairness.

## Shared payload utility

`steam/steamnetworkingsharedpayload.h` supplies `SteamNetworkingSharedPayload`.
Adopt immutable application storage, or copy it once, then create independent
GNS message descriptors for each recipient. Send them with the existing
`SendMessages` API. Each descriptor retains its own connection, lane, message
number, congestion/retransmission and session encryption state.

The release callback runs exactly once after the creator and all queued-message
references are released; it may run on a library thread and must not throw or
call back into GNS. The descriptor's `m_nUserData` and `m_pfnFreeData` are reserved
for this ownership. There is no new vtable method, message schema or wire extension.
The test exercises 1,024 descriptors and concurrent release.

## Encryption and protocol scope

Shared payloads reuse immutable plaintext storage. Each connection retains its
own AES-GCM encryption, authentication, congestion control and retransmission
state. This implementation does not share ciphertext or cryptographic keys,
and does not introduce a new wire format.

Sharing symmetric authentication keys among recipients would change the trust
model: any key holder could authenticate a forged message. Shared ciphertext
would require a separate protocol and security design covering sender
authentication, nonce uniqueness, replay protection, key distribution and
membership changes. It is outside the scope of this implementation.
