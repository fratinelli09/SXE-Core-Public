/* Library with an incomplete C contract: the engine must reject it saying what is missing. */
#include "sxe_strategy.h"

SXE_STRATEGY_EXPORT int sxe_strategy_api_version(void)
{
    return SXE_STRATEGY_API_VERSION;
}

SXE_STRATEGY_EXPORT void* sxe_strategy_create(const sxe_host* host)
{
    (void)host;
    return 0;
}
