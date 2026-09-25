/* The default reverse address map: empty.
 *
 * tools/gen_revmap.py generates one per build into the recompiler output directory,
 * where CMake picks it up ahead of this file.  This stub is what gets linked when it
 * has not run, and an empty map makes every lookup return its input -- correct for a
 * build that did not move, and a safe no-op otherwise, since the callers treat "no
 * answer" as "keep what you had" rather than as a translation.
 *
 * The table itself is two words per interval and runs to a few hundred KB, which is
 * why it is generated rather than checked in.
 */

#include "rn_revmap.h"

const rn_revmap_entry rn_revmap[1] = { { 0u, 0u } };
const unsigned rn_revmap_count = 0u;
