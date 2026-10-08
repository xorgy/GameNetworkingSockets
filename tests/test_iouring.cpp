#include "steamnetworkingsockets_iouring.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <unistd.h>
static void Check( bool value )
{
	if ( !value )
		std::abort();
}
struct State
{
	std::set<uint32_t> received;
	size_t packetBytes = 1400;
	uint64_t receiver = 0;
};
static void Receive( void *context, const CUringUDPSocketIO::Packet &packet )
{
	auto &state = *static_cast<State *>( context );
	Check( packet.size == state.packetBytes && packet.receiver == state.receiver );
	uint32_t number;
	std::memcpy( &number, packet.data, sizeof( number ) );
	Check( number < 8192 );
	const auto *bytes = static_cast<const unsigned char *>( packet.data );
	for ( size_t i = sizeof( number ); i < packet.size; ++i )
		Check( bytes[i] == static_cast<unsigned char>( number + i ) );
	state.received.insert( number );
}
int main()
{
	auto io = CUringUDPSocketIO::Create();
	if ( !io )
	{
		puts( "io_uring unavailable" );
		return 77;
	}
	int a = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 ),
		b = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
	Check( a >= 0 && b >= 0 );
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	Check( bind( a, reinterpret_cast<sockaddr *>( &address ), sizeof( address ) ) == 0 );
	Check( bind( b, reinterpret_cast<sockaddr *>( &address ), sizeof( address ) ) == 0 );
	socklen_t length = sizeof( address );
	Check( getsockname( b, reinterpret_cast<sockaddr *>( &address ), &length ) == 0 );
	auto sender = io->AddReceiver( a );
	auto receiver = io->AddReceiver( b );
	State state;
	state.receiver = receiver;
	uint32_t sent = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( state.received.size() < 4096 && std::chrono::steady_clock::now() < deadline )
	{
		for ( int i = 0; i < 64 && sent < 4096; ++i, ++sent )
		{
			unsigned char packet[1400];
			std::memcpy( packet, &sent, sizeof( sent ) );
			for ( size_t j = sizeof( sent ); j < sizeof( packet ); ++j )
				packet[j] = static_cast<unsigned char>( sent + j );
			iovec chunks[] = { { packet, 32 }, { packet + 32, 1024 }, { packet + 1056, 344 } };
			Check( io->QueueSend( sender, 3, chunks, reinterpret_cast<sockaddr *>( &address ), length ) );
			// QueueSend owns its vectors; stack storage can immediately change.
			std::memset( packet, 0, sizeof( packet ) );
		}
		io->Pump( Receive, &state );
		if ( !io->Supported() ) { puts( "Multishot recvmsg unavailable" ); return 77; }
		std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
	}
	auto stats = io->Stats();
	printf( "io_uring UDP: %zu/4096 packets, %llu sends, %llu RX errors, %llu TX errors, %llu starvation "
			"rearms\n",
			state.received.size(), (unsigned long long)stats.sends, (unsigned long long)stats.receiveErrors,
			(unsigned long long)stats.sendErrors, (unsigned long long)stats.starvations );
	Check( state.received.size() == 4096 && stats.receiveErrors == 0 && stats.sendErrors == 0 );
	// Exercise both storage fallbacks, then immediately overwrite the caller's
	// bytes/vectors while the send is still queued.
	unsigned char large[3000];
	uint32_t number = 4096;
	std::memcpy( large, &number, sizeof( number ) );
	for ( size_t i = sizeof( number ); i < sizeof( large ); ++i )
		large[i] = static_cast<unsigned char>( number + i );
	iovec vectors[12];
	for ( unsigned i = 0; i < 12; ++i )
		vectors[i] = { large + 250 * i, 250 };
	state.packetBytes = sizeof( large );
	Check( io->QueueSend( sender, 12, vectors, reinterpret_cast<sockaddr *>( &address ), length ) );
	std::memset( large, 0, sizeof( large ) );
	std::memset( vectors, 0, sizeof( vectors ) );
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( ( !state.received.count( number ) || io->Stats().sends < 4097 ) && std::chrono::steady_clock::now() < deadline )
	{
		io->Pump( Receive, &state );
		std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
	}
	Check( state.received.count( number ) == 1 && io->Stats().sends == 4097 );
	state.packetBytes = 1400;
	// Fill the bounded send pool without pumping, reject its overflow, then
	// consume every send completion. UDP may drop packets during this burst;
	// the invariant being tested is slot ownership and completion reuse.
	unsigned char packet[1400];
	number = 4097;
	std::memcpy( packet, &number, sizeof( number ) );
	for ( size_t i = sizeof( number ); i < sizeof( packet ); ++i )
		packet[i] = static_cast<unsigned char>( number + i );
	iovec chunk{ packet, sizeof( packet ) };
	uint64_t completed = io->Stats().sends;
	for ( unsigned i = 0; i < 4096; ++i )
		Check( io->QueueSend( sender, 1, &chunk, reinterpret_cast<sockaddr *>( &address ), length ) );
	Check( !io->QueueSend( sender, 1, &chunk, reinterpret_cast<sockaddr *>( &address ), length ) );
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( io->Stats().sends < completed + 4096 && std::chrono::steady_clock::now() < deadline )
	{
		io->Pump( Receive, &state );
		std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
	}
	Check( io->Stats().sends == completed + 4096 && io->Stats().sendErrors == 0 );
	// Detach and reuse the descriptor before the old receive's cancellation
	// completes. Old completions must never be dispatched to the new receiver.
	io->Detach( receiver );
	close( b );
	b = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
	Check( b >= 0 );
	address.sin_port = 0;
	Check( bind( b, reinterpret_cast<sockaddr *>( &address ), sizeof( address ) ) == 0 );
	Check( getsockname( b, reinterpret_cast<sockaddr *>( &address ), &length ) == 0 );
	receiver = io->AddReceiver( b );
	state.receiver = receiver;
	state.received.clear();
	completed = io->Stats().sends;
	Check( io->QueueSend( sender, 1, &chunk, reinterpret_cast<sockaddr *>( &address ), length ) );
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
	while ( ( state.received.empty() || io->Stats().sends < completed + 1 ) && std::chrono::steady_clock::now() < deadline )
	{
		io->Pump( Receive, &state );
		std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
	}
	Check( state.received.count( number ) == 1 && io->Stats().sends == completed + 1 );
	// Queued but not submitted sends are discarded on detach.
	completed = io->Stats().sends;
	Check( io->QueueSend( sender, 1, &chunk, reinterpret_cast<sockaddr *>( &address ), length ) );
	io->Detach( sender );
	io->Pump( Receive, &state );
	Check( io->Stats().sends == completed );
	io->Detach( receiver );
	close( a );
	close( b );
	io.reset();
	// Reinitialization and shutdown with receives still outstanding.
	io = CUringUDPSocketIO::Create();
	Check( bool( io ) );
	a = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
	Check( a >= 0 );
	sender = io->AddReceiver( a );
	io->Pump( nullptr, nullptr );
	io->Detach( sender );
	close( a );
	io.reset();
	puts( "io_uring multishot receive, owned vectored send, cancellation and reinitialization passed" );
}
