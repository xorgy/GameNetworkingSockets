// Distributed under the BSD-3-Clause license; see LICENSE.
#pragma once
#include "isteamnetworkingsockets.h"
#include "isteamnetworkingutils.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

/// Immutable application payload shared by independently queued messages.
/// Each connection retains its own reliability, congestion and encryption
/// state. This utility uses the existing message ownership API and does not
/// change the wire protocol or the ISteamNetworkingSockets ABI.
class SteamNetworkingSharedPayload final
{
public:
	/// Adopt immutable storage. ReleaseStorage runs exactly once, after the
	/// owner's reference and every queued message reference have been released.
	/// It can run on a library thread, must not throw, and must not call back
	/// into the library.
	static SteamNetworkingSharedPayload *Adopt( const void *data, int size,
												void ( *releaseStorage )( void * ), void *context )
	{
		if ( size < 0 || ( size && !data ) || !releaseStorage )
			return nullptr;
		return new ( std::nothrow ) SteamNetworkingSharedPayload( data, size, releaseStorage, context );
	}
	/// Copy once, then use CreateMessage for each recipient.
	static SteamNetworkingSharedPayload *Copy( const void *data, int size )
	{
		if ( size < 0 || ( size && !data ) )
			return nullptr;
		void *storage = std::malloc( size ? size : 1 );
		if ( !storage )
			return nullptr;
		if ( size )
			std::memcpy( storage, data, size );
		auto *result = Adopt( storage, size, &std::free, storage );
		if ( !result )
			std::free( storage );
		return result;
	}
	void AddRef() { m_refs.fetch_add( 1, std::memory_order_relaxed ); }
	void Release()
	{
		if ( m_refs.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
			delete this;
	}
	/// The caller owns the returned descriptor and must either pass it to
	/// SendMessages or Release it. Payload storage is not copied or modified.
	/// m_nUserData and m_pfnFreeData are reserved for payload ownership and
	/// must not be modified on this descriptor.
	SteamNetworkingMessage_t *CreateMessage( ISteamNetworkingUtils *utils, HSteamNetConnection connection,
											 int flags, uint16 lane = 0 )
	{
		if ( !utils )
			return nullptr;
		auto *message = utils->AllocateMessage( 0 );
		if ( !message )
			return nullptr;
		AddRef();
		message->m_pData = const_cast<void *>( m_data );
		message->m_cbSize = m_size;
		message->m_conn = connection;
		message->m_nFlags = flags;
		message->m_idxLane = lane;
		message->m_nUserData = reinterpret_cast<intptr_t>( this );
		message->m_pfnFreeData = FreeMessageData;
		return message;
	}

private:
	std::atomic<unsigned> m_refs{ 1 };
	const void *m_data;
	int m_size;
	void ( *m_releaseStorage )( void * );
	void *m_context;
	SteamNetworkingSharedPayload( const void *data, int size, void ( *release )( void * ), void *context )
		: m_data( data ), m_size( size ), m_releaseStorage( release ), m_context( context )
	{
	}
	~SteamNetworkingSharedPayload() { m_releaseStorage( m_context ); }
	static void FreeMessageData( SteamNetworkingMessage_t *message )
	{
		auto *payload =
			reinterpret_cast<SteamNetworkingSharedPayload *>( static_cast<intptr_t>( message->m_nUserData ) );
		message->m_pData = nullptr;
		message->m_nUserData = 0;
		payload->Release();
	}
	SteamNetworkingSharedPayload( const SteamNetworkingSharedPayload & ) = delete;
	SteamNetworkingSharedPayload &operator=( const SteamNetworkingSharedPayload & ) = delete;
};
