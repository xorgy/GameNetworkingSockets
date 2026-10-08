// Distributed under the BSD-3-Clause license; see LICENSE.
#pragma once
#ifdef STEAMNETWORKINGSOCKETS_USE_IO_URING
#include <cstdint>
#include <memory>
#include <sys/socket.h>
#include <sys/uio.h>

// Socket-thread-owned Linux UDP reactor. QueueSend/Detach may also be called
// with the library global lock held. Only Pump touches submission/completion
// rings, preserving SINGLE_ISSUER. Receive callbacks borrow packet storage
// until they return. Construction, Pump and destruction run on that same thread.
// QueueSend and Detach require the same external serialization as Pump.
// Supplied receive buffers are not the NIC zero-copy receive facility.
class CUringUDPSocketIO
{
public:
	struct Packet
	{
		uint64_t receiver;
		const void *data;
		size_t size;
		sockaddr_storage address;
		msghdr message;
	};
	struct Counters
	{
		uint64_t receives = 0, sends = 0, sendErrors = 0, receiveErrors = 0, starvations = 0, submissions = 0;
	};
	static std::unique_ptr<CUringUDPSocketIO> Create();
	~CUringUDPSocketIO();
	uint64_t AddReceiver( int fd );
	void Detach( uint64_t receiver );
	bool QueueSend( uint64_t receiver, int count, const iovec *chunks, const sockaddr *address,
					socklen_t length );
	void Pump( void ( *receive )( void *, const Packet & ), void *context );
	int WakeFD() const;
	bool Supported() const;
	const Counters &Stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	explicit CUringUDPSocketIO( std::unique_ptr<Impl> );
};
#endif
