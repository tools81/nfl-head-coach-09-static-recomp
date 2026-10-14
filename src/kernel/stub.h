#pragma once

#include <ppc_recomp_shared.h>

namespace hc09
{
    // Logs the first call of an unimplemented import, with the caller's return address.
    void ReportStubCall(const char* name, const char* library, const PPCContext& ctx, bool& reported);
}

#define HC09_IMPORT_STUB(name, library)                                  \
    PPC_FUNC(__imp__##name)                                              \
    {                                                                    \
        static bool reported = false;                                    \
        hc09::ReportStubCall(#name, library, ctx, reported);             \
        ctx.r3.u64 = 0;                                                  \
    }
