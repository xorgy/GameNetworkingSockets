// Distributed under the BSD-3-Clause license; see LICENSE.
#include <steam/steamnetworkingsockets.h>
#include "steamnetworkingsockets/steamnetworkingsockets_thinker.h"
#include "steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode( bool );
using namespace SteamNetworkingSocketsLib;
struct BusyThinker final : IThinker
{
    unsigned calls = 0;
    void Think( SteamNetworkingMicroseconds now ) override
    {
        ++calls;
        std::this_thread::sleep_for( std::chrono::microseconds(250) );
        SetNextThinkTime( now + 1 );
    }
};
int main()
{
    SteamNetworkingSockets_SetManualPollMode(true);
    SteamDatagramErrMsg error;
    if(!GameNetworkingSockets_Init(nullptr,error)) { std::puts(error);return 1; }
    {
        SteamNetworkingGlobalLock lock("Test service fairness");
        std::array<BusyThinker,8> thinkers;
        for(auto& thinker:thinkers)thinker.SetNextThinkTimeASAP();
        for(unsigned pass=0;pass<16;++pass)
        {
            unsigned before=0,after=0;
            for(const auto& thinker:thinkers)before+=thinker.calls;
            IThinker::Thinker_ProcessThinkers(1000);
            for(const auto& thinker:thinkers)after+=thinker.calls;
            if(after<=before || after-before>5)std::abort();
        }
        for(auto& thinker:thinkers)
        {
            if(!thinker.calls || !thinker.IsScheduled())std::abort();
            thinker.ClearNextThinkTime();
        }
    }
    GameNetworkingSockets_Kill();
    SteamNetworkingSockets_SetManualPollMode(false);
    std::puts("Bounded thinker passes preserve queued work and service every busy connection");
}
