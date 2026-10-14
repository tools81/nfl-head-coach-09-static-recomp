#pragma once

namespace hc09
{
    // Fills every import-table slot: function slots point at their thunks, data slots at
    // host-created kernel variables. Requires the XEX header to be loaded first.
    void PatchImportSlots();
}
