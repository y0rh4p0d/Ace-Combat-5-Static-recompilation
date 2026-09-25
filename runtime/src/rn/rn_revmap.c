#include "rn_revmap.h"

/* Where a reference address is, or its input when the map does not say.
 *
 * The table is sorted and disjoint -- tools/gen_revmap.py refuses to emit one that is
 * not -- so this is a plain binary search.  It runs once per emitter rather than once
 * per primitive, but it is cheap enough either way.
 *
 * A miss returns the input.  That is the right answer in two different situations:
 * the address is already a reference address (the build did not move, so nothing
 * needs translating), or the map genuinely does not cover it.  Both callers want to
 * carry on with what they had rather than be told nothing. */
u32 rn_revmap_to_us(u32 a) {
    unsigned lo = 0, hi = rn_revmap_count;
    if (!rn_revmap_count) return a;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (rn_revmap[mid].target <= a) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u) return a;
    {
        const rn_revmap_entry *e = &rn_revmap[lo - 1u];
        /* The interval ends where the next one starts. */
        u32 end = (lo < rn_revmap_count) ? rn_revmap[lo].target : 0xFFFFFFFFu;
        if (a < e->target || a >= end) return a;
        return a - e->delta;
    }
}
