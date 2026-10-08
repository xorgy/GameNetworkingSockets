#include <array>
#include <cstdio>
#include <cstdlib>
#include <steam/steamnetworkingsharedpayload.h>
#include <steam/steamnetworkingsockets.h>
#include <thread>
#include <vector>
static void Check( bool value )
{
	if ( !value )
		std::abort();
}
int main()
{
	SteamDatagramErrMsg error;
	Check( GameNetworkingSockets_Init( nullptr, error ) );
	auto *utils = SteamNetworkingUtils();
	std::array<unsigned char, 1400> bytes{};
	for ( size_t i = 0; i < bytes.size(); ++i )
		bytes[i] = static_cast<unsigned char>( i );
	std::atomic<int> releases{ 0 };
	auto *payload = SteamNetworkingSharedPayload::Adopt(
		bytes.data(), bytes.size(), []( void *context ) { ++*static_cast<std::atomic<int> *>( context ); },
		&releases );
	Check( payload != nullptr );
	std::vector<SteamNetworkingMessage_t *> messages;
	for ( int i = 0; i < 1024; ++i )
	{
		auto *message = payload->CreateMessage( utils, k_HSteamNetConnection_Invalid,
												k_nSteamNetworkingSend_Unreliable, 2 );
		Check( message && message->m_pData == bytes.data() && message->m_cbSize == int( bytes.size() ) &&
			   message->m_idxLane == 2 );
		messages.push_back( message );
	}
	payload->Release();
	Check( releases == 0 );
	// Descriptors may be released by multiple transport/application threads.
	std::thread a(
		[&]
		{
			for ( size_t i = 0; i < messages.size() / 2; ++i )
				messages[i]->Release();
		} );
	std::thread b(
		[&]
		{
			for ( size_t i = messages.size() / 2; i < messages.size(); ++i )
				messages[i]->Release();
		} );
	a.join();
	b.join();
	Check( releases == 1 );
	payload = SteamNetworkingSharedPayload::Copy( bytes.data(), bytes.size() );
	Check( payload != nullptr );
	messages.clear();
	for ( int i = 0; i < 16; ++i )
		messages.push_back(
			payload->CreateMessage( utils, k_HSteamNetConnection_Invalid, k_nSteamNetworkingSend_Reliable ) );
	payload->Release();
	std::array<int64, 16> results{};
	SteamNetworkingSockets()->SendMessages( messages.size(), messages.data(), results.data(), true );
	for ( auto result : results )
		Check( result == -k_EResultInvalidParam );
	Check( !SteamNetworkingSharedPayload::Copy( nullptr, 1 ) );
	Check( !SteamNetworkingSharedPayload::Copy( bytes.data(), -1 ) );
	GameNetworkingSockets_Kill();
	puts( "Shared payload: 1024 recipients, concurrent release, failed-send ownership passed" );
}
