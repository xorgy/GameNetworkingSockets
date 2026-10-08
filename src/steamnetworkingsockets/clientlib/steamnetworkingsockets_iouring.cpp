// Distributed under the BSD-3-Clause license; see LICENSE.
#include "steamnetworkingsockets_iouring.h"
#ifdef STEAMNETWORKINGSOCKETS_USE_IO_URING
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <liburing.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
namespace
{
constexpr unsigned kSends = 4096, kInlineBytes = 2048, kInlineChunks = 8;
constexpr unsigned kBuffers = 2048, kBufferBytes = 4096, kGroup = 1, kMask = kBuffers - 1;
constexpr uint64_t kSend = uint64_t( 1 ) << 63, kCancel = uint64_t( 1 ) << 62;
} // namespace
struct CUringUDPSocketIO::Impl
{
	struct Receiver
	{
		uint64_t id;
		int fd;
		bool active = false, closed = false, cancel = false;
		msghdr message{};
	};
	struct Send
	{
		unsigned id = 0;
		bool active = false;
		std::shared_ptr<Receiver> receiver;
		std::array<char, kInlineBytes> inlineBytes;
		std::array<iovec, kInlineChunks> inlineChunks;
		std::vector<char> bytes;
		std::vector<iovec> chunks;
		sockaddr_storage address{};
		msghdr message{};
	};
	io_uring ring{};
	io_uring_buf_ring *buffers = nullptr;
	void *area = nullptr;
	int wake = -1;
	uint64_t next = 1;
	Counters counters;
	bool initialized = false, supported = true;
	std::unordered_map<uint64_t, std::shared_ptr<Receiver>> receivers;
	// Slots are reused only after their send CQE is consumed. The kernel cookie
	// therefore indexes stable storage without a per-datagram allocation/map.
	std::array<std::unique_ptr<Send>, kSends> sends;
	std::vector<unsigned> available;
	std::deque<unsigned> queued;
	unsigned created = 0, sending = 0;
	void ReleaseSend( unsigned slot )
	{
		auto &send = *sends[slot];
		send.active = false;
		send.receiver.reset();
		available.push_back( slot );
	}
	~Impl()
	{
		if ( initialized )
		{
			// Finish cancellation and all sends before freeing message templates,
			// user buffers or descriptors; queue_exit alone is not our lifetime fence.
			for ( unsigned slot : queued )
				ReleaseSend( slot );
			queued.clear();
			for ( auto &item : receivers )
				item.second->closed = true;
			for ( ;; )
			{
				Prepare();
				Process( nullptr, nullptr );
				bool active = sending != 0;
				for ( const auto &item : receivers )
					active |= item.second->active || item.second->cancel;
				if ( !active )
					break;
				io_uring_cqe *cqe = nullptr;
				io_uring_wait_cqe( &ring, &cqe );
			}
			if ( buffers )
				io_uring_free_buf_ring( &ring, buffers, kBuffers, kGroup );
			io_uring_queue_exit( &ring );
		}
		if ( wake >= 0 )
			close( wake );
		std::free( area );
	}
	bool Submit()
	{
		++counters.submissions;
		int rc;
		do
		{
			rc = io_uring_submit( &ring );
		} while ( rc == -EINTR );
		return rc >= 0;
	}
	io_uring_sqe *SQE()
	{
		auto *sqe = io_uring_get_sqe( &ring );
		if ( !sqe )
		{
			if ( !Submit() )
				return nullptr;
			sqe = io_uring_get_sqe( &ring );
		}
		return sqe;
	}
	void Prepare()
	{
		for ( auto &item : receivers )
		{
			auto &r = *item.second;
			if ( r.closed )
			{
				if ( r.active && !r.cancel )
				{
					auto *sqe = SQE();
					if ( !sqe )
						continue;
					io_uring_prep_cancel64( sqe, r.id, 0 );
					io_uring_sqe_set_data64( sqe, kCancel | r.id );
					r.cancel = true;
				}
			}
			else if ( !r.active && supported )
			{
				auto *sqe = SQE();
				if ( !sqe )
					continue;
				io_uring_prep_recvmsg_multishot( sqe, r.fd, &r.message, 0 );
				sqe->flags |= IOSQE_BUFFER_SELECT;
				sqe->buf_group = kGroup;
				io_uring_sqe_set_data64( sqe, r.id );
				r.active = true;
			}
		}
		while ( !queued.empty() )
		{
			unsigned slot = queued.front();
			queued.pop_front();
			auto *send = sends[slot].get();
			if ( send->receiver->closed )
			{
				ReleaseSend( slot );
				continue;
			}
			auto *sqe = SQE();
			if ( !sqe )
			{
				queued.push_front( slot );
				break;
			}
			io_uring_prep_sendmsg( sqe, send->receiver->fd, &send->message, MSG_DONTWAIT );
			io_uring_sqe_set_data64( sqe, kSend | send->id );
			send->active = true;
			++sending;
		}
		Submit();
	}
	void Process( void ( *callback )( void *, const Packet & ), void *context )
	{
		uint64_t value;
		while ( read( wake, &value, sizeof( value ) ) >= 0 )
		{
		}
		unsigned head, count = 0, returned = 0;
		io_uring_cqe *cqe;
		io_uring_for_each_cqe( &ring, head, cqe )
		{
			uint64_t id = io_uring_cqe_get_data64( cqe );
			if ( id & kSend )
			{
				if ( cqe->res < 0 )
					++counters.sendErrors;
				else
					++counters.sends;
				unsigned slot = unsigned( id & ~kSend );
				assert( slot < created && sends[slot]->active );
				if ( slot >= created || !sends[slot]->active )
					std::abort();
				--sending;
				ReleaseSend( slot );
			}
			else if ( id & kCancel )
			{
				auto r = receivers.find( id & ~kCancel );
				if ( r != receivers.end() )
					r->second->cancel = false;
			}
			else
			{
				auto i = receivers.find( id );
				assert( i != receivers.end() );
				if ( i == receivers.end() )
					std::abort();
				auto &receiver = *i->second;
				if ( !( cqe->flags & IORING_CQE_F_MORE ) )
					receiver.active = false;
				if ( cqe->res < 0 )
				{
					if ( cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP || cqe->res == -ENOSYS )
						supported = false;
					if ( cqe->res == -ENOBUFS )
						++counters.starvations;
					else if ( cqe->res != -ECANCELED )
						++counters.receiveErrors;
				}
				if ( cqe->flags & IORING_CQE_F_BUFFER )
				{
					unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
					char *buffer = static_cast<char *>( area ) + bid * kBufferBytes;
					auto *out = cqe->res >= 0 ? io_uring_recvmsg_validate( buffer, cqe->res, &receiver.message ) : nullptr;
					if ( cqe->res >= 0 && out && out->namelen <= sizeof( sockaddr_storage ) &&
						 !( out->flags & ( MSG_TRUNC | MSG_CTRUNC ) ) && !receiver.closed && callback )
					{
						Packet packet{};
						packet.receiver = id;
						packet.data = io_uring_recvmsg_payload( out, &receiver.message );
						packet.size = io_uring_recvmsg_payload_length( out, cqe->res, &receiver.message );
						void *address = io_uring_recvmsg_name( out );
						std::memcpy( &packet.address, address,
									 std::min<size_t>( out->namelen, sizeof( packet.address ) ) );
						packet.message.msg_name = &packet.address;
						packet.message.msg_namelen = out->namelen;
						packet.message.msg_control = io_uring_recvmsg_cmsg_firsthdr( out, &receiver.message );
						packet.message.msg_controllen = out->controllen;
						packet.message.msg_flags = out->flags;
						++counters.receives;
						callback( context, packet );
					}
					io_uring_buf_ring_add( buffers, buffer, kBufferBytes, bid, kMask, returned++ );
				}
			}
			++count;
			if ( count == 512 )
				break;
		}
		if ( returned )
			io_uring_buf_ring_advance( buffers, returned );
		io_uring_cq_advance( &ring, count );
		if ( io_uring_cq_ready( &ring ) )
		{
			uint64_t one = 1;
			write( wake, &one, sizeof( one ) );
		}
		for ( auto i = receivers.begin(); i != receivers.end(); )
		{
			if ( i->second->closed && !i->second->active && !i->second->cancel )
				i = receivers.erase( i );
			else
				++i;
		}
	}
};
std::unique_ptr<CUringUDPSocketIO> CUringUDPSocketIO::Create()
{
	std::unique_ptr<Impl> impl( new Impl );
	io_uring_params params{};
	params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
	params.cq_entries = 8192;
	if ( io_uring_queue_init_params( 1024, &impl->ring, &params ) < 0 )
		return nullptr;
	impl->initialized = true;
	impl->available.reserve( kSends );
	auto *probe = io_uring_get_probe_ring( &impl->ring );
	bool operations = probe && io_uring_opcode_supported( probe, IORING_OP_RECVMSG ) &&
					  io_uring_opcode_supported( probe, IORING_OP_SENDMSG );
	io_uring_free_probe( probe );
	if ( !operations )
		return nullptr;
	impl->wake = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
	if ( impl->wake < 0 || io_uring_register_eventfd( &impl->ring, impl->wake ) < 0 )
		return nullptr;
	int error = 0;
	impl->buffers = io_uring_setup_buf_ring( &impl->ring, kBuffers, kGroup, 0, &error );
	if ( !impl->buffers || posix_memalign( &impl->area, 4096, kBuffers * kBufferBytes ) )
		return nullptr;
	for ( unsigned i = 0; i < kBuffers; ++i )
		io_uring_buf_ring_add( impl->buffers, static_cast<char *>( impl->area ) + i * kBufferBytes,
							   kBufferBytes, i, kMask, i );
	io_uring_buf_ring_advance( impl->buffers, kBuffers );
	return std::unique_ptr<CUringUDPSocketIO>( new CUringUDPSocketIO( std::move( impl ) ) );
}
CUringUDPSocketIO::CUringUDPSocketIO( std::unique_ptr<Impl> impl ) : m_impl( std::move( impl ) ) {}
CUringUDPSocketIO::~CUringUDPSocketIO() = default;
uint64_t CUringUDPSocketIO::AddReceiver( int fd )
{
	auto receiver = std::make_shared<Impl::Receiver>();
	receiver->id = m_impl->next++;
	receiver->fd = fd;
	receiver->message.msg_namelen = sizeof( sockaddr_storage );
	receiver->message.msg_controllen = 64;
	m_impl->receivers.emplace( receiver->id, receiver );
	return receiver->id;
}
void CUringUDPSocketIO::Detach( uint64_t receiver )
{
	auto found = m_impl->receivers.find( receiver );
	if ( found != m_impl->receivers.end() )
		found->second->closed = true;
}
bool CUringUDPSocketIO::QueueSend( uint64_t receiver, int count, const iovec *chunks, const sockaddr *address,
								   socklen_t length )
{
	auto found = m_impl->receivers.find( receiver );
	if ( found == m_impl->receivers.end() || found->second->closed || count < 1 || count > 1024 || !chunks ||
		 !address || length > sizeof( sockaddr_storage ) ||
		 m_impl->queued.size() + m_impl->sending >= kSends )
		return false;
	size_t size = 0;
	for ( int i = 0; i < count; ++i )
	{
		if ( chunks[i].iov_len > 65536 - size )
			return false;
		size += chunks[i].iov_len;
	}
	unsigned slot;
	if ( m_impl->available.empty() )
	{
		slot = m_impl->created++;
		m_impl->sends[slot].reset( new Impl::Send );
		m_impl->sends[slot]->id = slot;
	}
	else
	{
		slot = m_impl->available.back();
		m_impl->available.pop_back();
	}
	auto *send = m_impl->sends[slot].get();
	send->receiver = found->second;
	char *bytes = send->inlineBytes.data();
	if ( size > kInlineBytes )
	{
		send->bytes.resize( size );
		bytes = send->bytes.data();
	}
	iovec *vectors = send->inlineChunks.data();
	if ( count > int( kInlineChunks ) )
	{
		send->chunks.resize( count );
		vectors = send->chunks.data();
	}
	send->message = {};
	size_t offset = 0;
	for ( int i = 0; i < count; ++i )
	{
		auto length = chunks[i].iov_len;
		if ( length )
			std::memcpy( bytes + offset, chunks[i].iov_base, length );
		vectors[i] = { bytes + offset, length };
		offset += length;
	}
	std::memcpy( &send->address, address, length );
	send->message.msg_name = &send->address;
	send->message.msg_namelen = length;
	send->message.msg_iov = vectors;
	send->message.msg_iovlen = count;
	bool wake = m_impl->queued.empty();
	m_impl->queued.push_back( slot );
	if ( wake )
	{
		uint64_t one = 1;
		write( m_impl->wake, &one, sizeof( one ) );
	}
	return true;
}
void CUringUDPSocketIO::Pump( void ( *receive )( void *, const Packet & ), void *context )
{
	m_impl->Prepare();
	m_impl->Process( receive, context );
	m_impl->Prepare();
}
bool CUringUDPSocketIO::Supported() const { return m_impl->supported; }
int CUringUDPSocketIO::WakeFD() const { return m_impl->wake; }
const CUringUDPSocketIO::Counters &CUringUDPSocketIO::Stats() const { return m_impl->counters; }
#endif
