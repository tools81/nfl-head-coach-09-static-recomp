#pragma once

// HC27: the real owners of a career's 2028 draft picks.
//
// A new career takes its draft picks (the DRPK table: the next two drafts) from a template on the
// disc, which tools/roster_import/career_picks.py replaces with the real 2027 trades (hc27\disc).
// A pre-season start makes the draft after that, 2028, itself, with every team holding its own
// picks; hc27\draft_picks.txt lists the trades of 2028 picks, which the career gets when it is
// first saved: in memory (ApplyDraftPickTrades, so the rest of the session has them) and in the save
// file once written (ApplyDraftPickTradesToSave: the game may write a copy of its database made before
// the first could reach it).

#include <filesystem>

namespace hc09::edition
{
    // Called when the game creates a career save, before it writes the career. Finds the careers'
    // pick tables in guest memory and applies the 2028 trades to one whose next draft is 2027 and
    // whose 2028 picks all still belong to their own teams (so once, and never after the user has
    // traded any). Does nothing in HC09.
    void ApplyDraftPickTrades();

    // Called when the game closes a career save it wrote: the same for the saved file (its pick table,
    // the table's CRC and the save's checksums). Does nothing in HC09.
    void ApplyDraftPickTradesToSave(const std::filesystem::path& file);
}
