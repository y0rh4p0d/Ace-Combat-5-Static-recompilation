#include "ps2_hle.h"
#include "ps2_hook.h"
#include "ps2_gfxq.h"
#include "ps2_capture.h"
#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_taps_on;

#define F_DB_OPEN    0x00320098u
#define F_DB_CLOSE   0x003200D8u
#define F_OT_INIT    0x00320390u
#define F_OT_OPEN    0x00320538u
#define F_OT_CLOSE   0x003205D0u
#define F_OT_LINK    0x00320648u
#define F_DC_FLUSH   0x003265E8u
#define F_SCENE_DISPATCH 0x0031CF00u
#define A_DRAWCTRL_PTR   0x004459A8u
#define A_SCENE_ROOT_PTR 0x004459ACu

typedef struct { u32 addr, size; u32 w[4]; } known_fn;

/* Region-independent addressing.
 *
 * Everything below is written in US (SLUS_208.51) addresses, and the Japanese
 * executable -- which is also the base for the Chinese localisation -- puts the
 * same functions at different addresses.  The shift is not one constant: the
 * draw-control and 2D helpers move by +0x328, the sun writers by +8, another by
 * -0x28.  So each entry carries a signature of its leading instructions, and
 * rn_resolve() scans the loaded image for them.
 *
 * Two anchors have signatures that are unique in both builds.  Once they are
 * found the shift is known exactly, which is what disambiguates the entries
 * whose own signatures repeat (two of the 2D helpers are byte-identical at
 * entry, and one shares its prologue with an unrelated function elsewhere).
 */
typedef struct { u32 us_addr; int n; u32 sig[8]; u32 mask[8]; } tap_sig;

static const tap_sig tap_sigs[] = {
    /* Generated from SLUS_208.51 by tools/gen_tap_sigs.py.  Do not hand-edit:
     * a single wrong mask nibble makes an entry silently unmatchable.  A mask of 0
     * means the word is skipped, because its immediate or branch target is an
     * address that moves between builds. */
    { 0x00320098u, 8,
      { 0x27BD0000u, 0xFFBF0000u, 0x80820000u, 0x10400000u, 0x24030000u, 0x3C040000u, 0x0C000000u, 0x24840000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFC000000u, 0xFFFF0000u } },  /* F_DB_OPEN */
    { 0x003200D8u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0xFFB10000u, 0xFFBF0000u, 0x82020000u, 0x14400000u, 0x00A0882Du },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu } },  /* F_DB_CLOSE */
    { 0x00320390u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0x00A0202Du, 0xFFBF0000u, 0x0C000000u, 0xAE040000u, 0x8E040000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0xFC000000u, 0xFFFF0000u, 0xFFFF0000u } },  /* F_OT_INIT */
    { 0x00320538u, 8,
      { 0x27BD0000u, 0x24030000u, 0xFFB00000u, 0xFFBF0000u, 0x8C820000u, 0x10430000u, 0x00000000u, 0x3C040000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_OT_OPEN */
    { 0x003205D0u, 8,
      { 0x27BD0000u, 0x0080382Du, 0xFFBF0000u, 0x00A0302Du, 0x24080000u, 0x24C50000u, 0x8CE20000u, 0x14480000u },
      { 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u } },  /* F_OT_CLOSE */
    { 0x00320648u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0xFFBF0000u, 0x0C000000u, 0x8E040000u, 0x8E030000u, 0x3C050000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFC000000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u } },  /* F_OT_LINK */
    { 0x003265E8u, 8,
      { 0x27BD0000u, 0xFFB70000u, 0x0080B82Du, 0xFFB00000u, 0xFFB10000u, 0xFFB20000u, 0xFFB30000u, 0xFFB40000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u } },  /* F_DC_FLUSH */
    { 0x0032B3F0u, 8,
      { 0x27BD0000u, 0x3C010000u, 0x44812000u, 0xE7B40000u, 0x24030000u, 0x3C010000u, 0x4481A000u, 0x24020000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_2D_0 */
    { 0x0032B5B0u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0xFFB10000u, 0x00A0882Du, 0xFFB20000u, 0x00C0902Du, 0xFFB30000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_2D_1 */
    { 0x0032B650u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0xFFB10000u, 0x00A0882Du, 0xFFB20000u, 0x00C0902Du, 0xFFB30000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_2D_2 */
    { 0x0032B6F0u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x00A0802Du, 0xFFB10000u, 0x0080882Du, 0xFFB20000u, 0x27B20000u, 0xFFBF0000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u } },  /* F_2D_3 */
    { 0x0032B850u, 8,
      { 0x27BD0000u, 0xFFB00000u, 0x0080802Du, 0xFFB10000u, 0x26110000u, 0xFFBF0000u, 0x03A0202Du, 0x92020000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_2D_4 */
    { 0x0032B928u, 8,
      { 0x27BD0000u, 0x00063400u, 0xFFB00000u, 0x00068403u, 0xFFB10000u, 0x30B10000u, 0xFFB20000u, 0x0080902Du },
      { 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu } },  /* F_2D_5 */
    { 0x0011AAF0u, 8,
      { 0x27BD0000u, 0x3C030000u, 0xFFBE0000u, 0x00A0F02Du, 0xE7B40000u, 0x27C20000u, 0x3C010000u, 0x4481A000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu } },  /* W_0 */
    { 0x00118BC8u, 8,
      { 0x27BD0000u, 0xFFB70000u, 0x0080B82Du, 0x03A0202Du, 0xFFB20000u, 0xFFB40000u, 0x00A0A02Du, 0xE7B50000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* W_1 */
    { 0x001B0680u, 8,
      { 0x27BD0000u, 0xFFB20000u, 0x27B20000u, 0xFFB30000u, 0x00A0982Du, 0xFFB40000u, 0x0080A02Du, 0xE7B50000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* W_2 */
    { 0x0031CF00u, 8,
      { 0x27BD0000u, 0xFFBF0000u, 0x90820000u, 0x8C850000u, 0x00021080u, 0x90830000u, 0x00451021u, 0x8C440000u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFF0000u } },  /* F_SCENE_DISP */
    /* Not tapped from here, but located by the same scan so that rn_fixes.c
     * does not have to carry its own US-only address. */
    { 0x00118BC8u, 8,
      { 0x27BD0000u, 0xFFB70000u, 0x0080B82Du, 0x03A0202Du, 0xFFB20000u, 0x0u, 0x0u, 0x0u },
      { 0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0x0u, 0x0u, 0x0u } },  /* F_SUN_FLARE */
};
#define N_SIGS (sizeof tap_sigs / sizeof tap_sigs[0])
static u32 tap_addr[N_SIGS];
static int tap_resolved;

/* Return the address of the entry whose US address is `us`, or 0. */
static u32 tap_of(u32 us) {
    for (size_t i = 0; i < N_SIGS; i++)
        if (tap_sigs[i].us_addr == us) return tap_addr[i];
    return 0;
}

/* Public form, for the other parts of the renderer that used to hardcode a US
 * address.  Falls back to the US address itself if the table was never resolved
 * (which keeps the original behaviour for the build the table was written for). */
u32 rn_resolve_addr(u32 us) {
    u32 a;
    if (!tap_resolved) return us;
    a = tap_of(us);
    return a ? a : us;
}

static int sig_matches(u32 at, const tap_sig *s) {
    for (int i = 0; i < s->n; i++) {
        if (s->mask[i] == 0) continue;            /* volatile field */
        /* Read the loaded executable, not guest RAM: the guest overwrites parts
         * of its own image, and one of the 2D helpers sits in a region that gets
         * paged over, which made this look like a missing function. */
        if ((ps2_image_word(at + 4u * (u32)i) & s->mask[i]) != s->sig[i])
            return 0;
    }
    return 1;
}

/* Count the matches for one entry inside the loaded image and, when asked,
 * record their addresses.  `out` may be NULL.
 *
 * Candidates are restricted to addresses that look like a function entry, which
 * both cuts the scan and rules out a signature that happens to appear in the
 * middle of a function or in a data table.  A recompiled-function lookup is not
 * enough on its own: the recompiler only records an address as a function when
 * the call graph reaches it, and several of these are reached only indirectly.
 */
static int looks_like_entry(u32 a) {
    u32 w = ps2_image_word(a);
    /* `addiu sp, sp, -N': a frame prologue, which every function the taps care
     * about begins with.  Deliberately not "or an address the recompiler knows",
     * because that answer depends on which executable is loaded and made the
     * candidate set differ between builds for no good reason. */
    return (w >> 26) == 0x09u && ((w >> 21) & 0x1Fu) == 29u
           && ((w >> 16) & 0x1Fu) == 29u && (w & 0x8000u);
}

static int sig_scan(const tap_sig *s, u32 *out, int cap) {
    u32 lo, hi;
    int n = 0;
    ps2_text_bounds(&lo, &hi);
    for (u32 a = lo; a + 4u * (u32)s->n <= hi; a += 4u) {
        if (!looks_like_entry(a)) continue;
        if (!sig_matches(a, s)) continue;
        /* Store up to cap but always keep counting: an early "found more than
         * one, stop" shortcut here used to truncate the total, which made the
         * caller believe a table with three matches had only two and then read a
         * buffer that had never been filled. */
        if (out && n < cap) out[n] = a;
        n++;
    }
    return n;
}

/* All candidate addresses for one entry, up to `cap`.  The total count is
 * returned even when it exceeds `cap`, so the caller can tell "the buffer filled
 * up" from "there really are only this many". */
#define TAP_CAND_MAX 128
static int sig_candidates(const tap_sig *s, u32 *out, int cap) {
    int total = sig_scan(s, out, cap);
    return total;
}

/* Find where each tap lives in whatever executable was loaded.
 *
 * There is no single address shift: these functions sit in three separate parts
 * of .text and each part moved by a different amount (+0x328 for the
 * draw-control cluster, +0x8 for one sun writer, -0x28 for another).  So instead
 * of one global shift we collect the shift of every entry whose signature is
 * unique, then use those to disambiguate the entries whose signatures repeat.
 * The repeats all sit inside a cluster whose shift is already known from a
 * unique neighbour, which is what makes this work. */
static int rn_resolve(void) {
    u32 cand[TAP_CAND_MAX];
    u32 known_shift[8];
    int n_known = 0;
    int progressed = 1;

    if (tap_resolved) return 1;

    /* Several entries only become resolvable once a neighbour's shift is known,
     * and the table is in address order rather than dependency order, so keep
     * going round until a pass resolves nothing new. */
    for (int pass = 0; pass < 8 && progressed; pass++) {
        progressed = 0;
        for (size_t i = 0; i < N_SIGS; i++) {
            const tap_sig *s = &tap_sigs[i];
            int n;
            if (tap_addr[i]) continue;
            n = sig_candidates(s, cand, TAP_CAND_MAX);
            if (n == 1) {
                tap_addr[i] = cand[0];
            } else {
                /* Ambiguous: only accept a candidate whose shift is already
                 * known.  Guessing here would add a bogus shift to the set and
                 * then "confirm" itself on a later pass. */
                for (int k = 0; k < n && !tap_addr[i]; k++) {
                    u32 d = cand[k] - s->us_addr;
                    for (int j = 0; j < n_known; j++)
                        if (known_shift[j] == d) { tap_addr[i] = cand[k]; break; }
                }
            }
            if (!tap_addr[i]) continue;
            progressed = 1;
            {
                u32 d = tap_addr[i] - s->us_addr;
                int seen = 0;
                for (int j = 0; j < n_known; j++)
                    if (known_shift[j] == d) seen = 1;
                if (!seen && n_known < (int)(sizeof known_shift / sizeof known_shift[0]))
                    known_shift[n_known++] = d;
            }
        }
    }

    /* Still unresolved: two of the 2D helpers are byte-identical at their entry
     * to each other and to an unrelated function further down .text, so no
     * signature separates them.  The address shift is piecewise constant and
     * every entry here has a resolved neighbour inside its own piece, so accept
     * the candidate sitting at the nearest resolved neighbour's shift.  In the
     * build the table was written for that shift is 0, which picks the original
     * address exactly. */
    for (size_t i = 0; i < N_SIGS; i++) {
        const tap_sig *s = &tap_sigs[i];
        int n;
        u32 want;
        if (tap_addr[i]) continue;
        n = sig_candidates(s, cand, TAP_CAND_MAX);
        if (!n || n > TAP_CAND_MAX) {
            ps2_log("rn: tap %08X %s; that tap is left to the emulated path",
                    s->us_addr,
                    n ? "matches too many places to be identified" : "is not present in this executable");
            continue;
        }
        {
            u32 near_d = 0, near_dist = ~0u;
            int have_neighbour = 0;
            for (size_t j = 0; j < N_SIGS; j++) {
                u32 dist;
                if (!tap_addr[j] || j == i) continue;
                dist = s->us_addr > tap_sigs[j].us_addr
                           ? s->us_addr - tap_sigs[j].us_addr
                           : tap_sigs[j].us_addr - s->us_addr;
                if (dist < near_dist) {
                    near_dist = dist;
                    near_d = tap_addr[j] - tap_sigs[j].us_addr;
                    have_neighbour = 1;
                }
            }
            if (!have_neighbour) continue;
            want = s->us_addr + near_d;
        }
        /* Accept only a candidate at exactly the neighbour's shift.  Choosing
         * "nearest" instead would happily pick a look-alike function from a
         * different part of .text. */
        for (int k = 0; k < n; k++)
            if (cand[k] == want) { tap_addr[i] = cand[k]; break; }
        if (!tap_addr[i]) {
            char buf[160];
            int m = 0;
            for (int k = 0; k < n && k < 6; k++)
                m += snprintf(buf + m, sizeof buf - (size_t)m, " %08X", cand[k]);
            ps2_log("rn: tap %08X: want %08X, %d candidate(s):%s; left to the "
                    "emulated path", s->us_addr, want, n, buf);
        }
    }

    {
        char buf[200];
        int n = 0;
        for (size_t i = 0; i < N_SIGS && n < (int)sizeof buf - 12; i++)
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %08X", tap_addr[i]);
        ps2_log("rn: %d address region(s); tap addresses:%s", n_known, buf);
    }
    tap_resolved = 1;
    return 1;
}

/* The functions below are the ones the taps hook, in US addresses.  Only the
 * sizes and one signature (for the sanity check after resolution) stay here; the
 * address itself comes from tap_of(), which rn_resolve() filled in.
 *
 * These are deliberately not const: the address is rewritten in place once the
 * shift for the loaded executable is known, and writing through a cast to a
 * const-qualified object is exactly the kind of thing an optimiser is entitled
 * to fold away. */
static known_fn fn_db_open  = { F_DB_OPEN,  0x3C,
    { 0x27BDFFF0u, 0xFFBF0000u, 0x80820015u, 0x10400006u } };
static known_fn fn_db_close = { F_DB_CLOSE, 0x94,
    { 0x27BDFFE0u, 0xFFB00000u, 0x0080802Du, 0xFFB10008u } };
static known_fn fn_ot_init  = { F_OT_INIT,  0x94,
    { 0x27BDFFF0u, 0xFFB00000u, 0x0080802Du, 0x00A0202Du } };
static known_fn fn_ot_open  = { F_OT_OPEN,  0x94,
    { 0x27BDFFF0u, 0x2403FFFFu, 0xFFB00000u, 0xFFBF0008u } };
static known_fn fn_ot_close = { F_OT_CLOSE, 0x74,
    { 0x27BDFFF0u, 0x0080382Du, 0xFFBF0000u, 0x00A0302Du } };
static known_fn fn_ot_link  = { F_OT_LINK,  0x4C,
    { 0x27BDFFF0u, 0xFFB00000u, 0x0080802Du, 0xFFBF0008u } };
static known_fn fn_dc_flush = { F_DC_FLUSH, 0xF14,
    { 0x27BDFEE0u, 0xFFB70108u, 0x0080B82Du, 0xFFB000D0u } };
static known_fn fn_2d[] = {
    { 0x0032B3F0u, 0x1C0, { 0x27BDFDF0u, 0x3C014420u, 0x44812000u, 0xE7B40208u } },
    { 0x0032B5B0u, 0x0A0, { 0x27BDFFC0u, 0xFFB00010u, 0x0080802Du, 0xFFB10018u } },
    { 0x0032B650u, 0x0A0, { 0x27BDFFC0u, 0xFFB00010u, 0x0080802Du, 0xFFB10018u } },
    { 0x0032B6F0u, 0x160, { 0x27BDFF50u, 0xFFB00090u, 0x00A0802Du, 0xFFB10098u } },
    { 0x0032B850u, 0x0D4, { 0x27BDFE80u, 0xFFB00110u, 0x0080802Du, 0xFFB10118u } },
    { 0x0032B928u, 0x090, { 0x27BDFFD0u, 0x00063400u, 0xFFB00000u, 0x00068403u } },
};
#define N_2D (sizeof fn_2d / sizeof fn_2d[0])

typedef struct { known_fn fn; int arg; } writer_fn;
static writer_fn writers[] = {
    { { 0x0011AAF0u, 0xD3C, { 0x27BDFF20u, 0x3C031000u, 0xFFBE00C0u, 0x00A0F02Du } }, 1 },
    { { 0x00118BC8u, 0xFAC, { 0x27BDFB70u, 0xFFB70458u, 0x0080B82Du, 0x03A0202Du } }, 1 },
    { { 0x001B0680u, 0x36C, { 0x27BDFE60u, 0xFFB20170u, 0x27B20040u, 0xFFB30178u } }, 1 },
};
#define N_WRITERS (sizeof writers / sizeof writers[0])

/* Point a table entry at wherever its function actually is.  `size` comes from
 * the table and is the same in every build because the code is the same.
 * Returns 0 when the function could not be located, in which case the entry is
 * left alone and its callers skip it. */
static int retarget(known_fn *f) {
    u32 a = tap_of(f->addr);
    if (a) f->addr = a;
    return a != 0;
}

/* Bit per entry in tap_sigs order that resolved, so the hook install can skip
 * the ones that did not. */
static u32 resolved_mask;

static void addrs_apply(void) {
    retarget(&fn_db_open);   resolved_mask |= tap_of(F_DB_OPEN)   ? 1u << 0 : 0u;
    retarget(&fn_db_close);  resolved_mask |= tap_of(F_DB_CLOSE)  ? 1u << 1 : 0u;
    retarget(&fn_ot_init);   resolved_mask |= tap_of(F_OT_INIT)   ? 1u << 2 : 0u;
    retarget(&fn_ot_open);   resolved_mask |= tap_of(F_OT_OPEN)   ? 1u << 3 : 0u;
    retarget(&fn_ot_close);  resolved_mask |= tap_of(F_OT_CLOSE)  ? 1u << 4 : 0u;
    retarget(&fn_ot_link);   resolved_mask |= tap_of(F_OT_LINK)   ? 1u << 5 : 0u;
    retarget(&fn_dc_flush);  resolved_mask |= tap_of(F_DC_FLUSH)  ? 1u << 6 : 0u;
    for (size_t i = 0; i < N_2D; i++)
        resolved_mask |= retarget(&fn_2d[i]) ? 1u << (7 + i) : 0u;
    for (size_t i = 0; i < N_WRITERS; i++)
        resolved_mask |= retarget(&writers[i].fn) ? 1u << (13 + i) : 0u;
    resolved_mask |= tap_of(F_SCENE_DISPATCH) ? 1u << 16 : 0u;
}

#define RESOLVED(bit) ((resolved_mask >> (bit)) & 1u)

/* The scene dispatcher is not in `writers`/`fn_2d`, so it needs its own name. */
static u32 scene_dispatch_addr(void) { return tap_of(F_SCENE_DISPATCH); }

static int code_matches(const known_fn *f) {
    for (int i = 0; i < 4; i++)
        if (ps2_r32(f->addr + 4u * (u32)i) != f->w[i]) return 0;
    return 1;
}

static inline int in_fn(const known_fn *f, u32 a) {
    return a - f->addr < f->size;
}

#define SITE_HASH 8192u
static struct { u32 site, id; } site_hash[SITE_HASH];
static int log_new = -1;

static u32 func_of(u32 site) {
    unsigned lo = 0, hi = ps2_func_count;
    if (!ps2_func_count) return 0;
    while (hi - lo > 1u) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (ps2_func_table[mid].addr <= site) lo = mid;
        else hi = mid;
    }
    return ps2_func_table[lo].addr <= site ? ps2_func_table[lo].addr : 0u;
}

static u32 emitter_for(u32 site, u32 via) {
    u32 h = (site * 2654435761u) >> 19;
    for (u32 probe = 0; probe < SITE_HASH; probe++) {
        u32 k = (h + probe) & (SITE_HASH - 1u);
        if (site_hash[k].site == site && site_hash[k].id) return site_hash[k].id;
        if (!site_hash[k].id) {
            u32 id = rn_emitter_count();
            if (id >= RN_EMIT_MAX) return RN_EMIT_NONE;
            rn_emitter_define(id, site, func_of(site), via);
            site_hash[k].site = site;
            site_hash[k].id = id;
            if (log_new < 0) log_new = PS2_ENV("PS2_RN_LOG") ? 1 : 0;
            if (log_new) {
                char nm[160];
                ps2_log("rn: emitter #%u %s (site %08X)", id,
                        rn_emitter_name(id, nm, sizeof nm), site);
            }
            return id;
        }
    }
    return RN_EMIT_NONE;
}

#define RANGE_MAX 32768u
typedef struct { u32 start, end, emitter; } range;
static range ranges[RANGE_MAX];
static u32 n_ranges;
static int ranges_sorted = 1;
static int ranges_full_said;

#define OPEN_MAX 8u
static struct {
    u32 buf;
    u32 start;
    u32 emitter;
} opens[OPEN_MAX];

static struct { u32 buf; } bufs[OPEN_MAX];

static u32 ot_caller, ot_table, ot_bucket;
static u32 twod_caller[N_2D];

static struct { u32 lo, hi; } dc_ranges[4];
static int in_flush;

static u64 st_frames, st_ranges, st_bytes, st_tags, st_tags_by[RN_EMIT_FIRST + 1];
static u64 st_overflow_frames, st_inherited;
static u32 st_peak_ranges;

static void add_range(u32 start, u32 end, u32 emitter) {
    if (end <= start) return;
    if (n_ranges >= RANGE_MAX) {
        if (!ranges_full_said++)
            ps2_log("rn: more than %u packet ranges in one frame; the rest are "
                    "unattributed", RANGE_MAX);
        return;
    }
    if (n_ranges && start < ranges[n_ranges - 1].start) ranges_sorted = 0;
    ranges[n_ranges].start = start;
    ranges[n_ranges].end = end;
    ranges[n_ranges].emitter = emitter;
    n_ranges++;
    st_ranges++;
    st_bytes += end - start;
    {   rn_emitter *e = rn_emitter_mut(emitter);
        if (e) { e->ranges++; e->bytes += end - start; } }
}

static int by_start(const void *a, const void *b) {
    const range *x = (const range *)a, *y = (const range *)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->end > y->end ? -1 : x->end < y->end;
}

static u32 lookup(u32 tadr) {
    if (!tadr) return RN_EMIT_NONE;
    if (!ranges_sorted) {
        qsort(ranges, n_ranges, sizeof ranges[0], by_start);
        ranges_sorted = 1;
    }
    if (n_ranges) {
        u32 lo = 0, hi = n_ranges;
        while (hi - lo > 1u) {
            u32 mid = lo + (hi - lo) / 2u;
            if (ranges[mid].start <= tadr) lo = mid;
            else hi = mid;
        }
        for (u32 k = 0; k <= lo && k < 32u; k++) {
            const range *r = &ranges[lo - k];
            if (r->start <= tadr && tadr < r->end) return r->emitter;
        }
    }
    for (int i = 0; i < 4; i++)
        if (dc_ranges[i].hi && tadr - dc_ranges[i].lo < dc_ranges[i].hi - dc_ranges[i].lo)
            return RN_EMIT_DRAWCTRL;
    for (u32 i = 0; i < OPEN_MAX; i++) {
        u32 b = bufs[i].buf, size, base0, base1;
        if (!b) continue;
        base0 = ps2_r32(b);
        base1 = ps2_r32(b + 4u);
        size = ps2_r32(b + 12u);
        if (tadr - base0 < size || tadr - base1 < size) return RN_EMIT_UNOWNED;
    }
    return in_flush ? RN_EMIT_NONE : RN_EMIT_OFFCHAIN;
}

static int tap_ot_open(ps2_ctx *ctx, void *u) {
    (void)u;
    ot_caller = (u32)ctx->r[31].ud[0];
    ot_table = ps2_arg(ctx, 0);
    ot_bucket = ps2_arg(ctx, 1);
    return 0;
}

static int tap_2d(ps2_ctx *ctx, void *u) {
    twod_caller[(uintptr_t)u] = (u32)ctx->r[31].ud[0];
    return 0;
}

static int tap_db_open(ps2_ctx *ctx, void *u) {
    u32 buf = ps2_arg(ctx, 0);
    u32 ra = (u32)ctx->r[31].ud[0];
    u32 site = ra, via = RN_VIA_DIRECT, emitter;
    u32 slot = OPEN_MAX;
    (void)u;
    if (in_fn(&fn_ot_open, ra)) {
        site = ot_caller;
        via = RN_VIA_OT;
    } else if (in_fn(&fn_ot_init, ra) || in_fn(&fn_ot_link, ra)) {
        site = 0;
    } else {
        for (u32 i = 0; i < N_2D; i++)
            if (in_fn(&fn_2d[i], ra)) { site = twod_caller[i]; via = RN_VIA_2D; break; }
    }
    emitter = site ? emitter_for(site, via) : RN_EMIT_OTHEAD;
    if (via == RN_VIA_OT) {
        rn_emitter *e = rn_emitter_mut(emitter);
        if (e) { e->bucket = ot_bucket; e->table = ot_table; }
    }
    for (u32 i = 0; i < OPEN_MAX; i++) {
        if (opens[i].buf == buf) { slot = i; break; }
        if (!opens[i].buf && slot == OPEN_MAX) slot = i;
    }
    if (slot == OPEN_MAX) return 0;
    opens[slot].buf = buf;
    opens[slot].start = ps2_r32(buf + 8u);
    opens[slot].emitter = emitter;
    for (u32 i = 0; i < OPEN_MAX; i++) {
        if (bufs[i].buf == buf) break;
        if (!bufs[i].buf) { bufs[i].buf = buf; break; }
    }
    return 0;
}

static int tap_db_close(ps2_ctx *ctx, void *u) {
    u32 buf = ps2_arg(ctx, 0), end = ps2_arg(ctx, 1);
    (void)u;
    for (u32 i = 0; i < OPEN_MAX; i++) {
        if (opens[i].buf != buf) continue;
        add_range(opens[i].start, end, opens[i].emitter);
        opens[i].buf = 0;
        break;
    }
    return 0;
}

static int tap_writer(ps2_ctx *ctx, void *u) {
    const writer_fn *w = &writers[(uintptr_t)u];
    int ok = 0;
    u32 start = ps2_hook_entry_arg(w->arg, &ok);
    u32 end = (u32)ctx->r[2].ud[0];
    if (!ok) return 0;
    add_range(start, end, emitter_for(w->fn.addr, RN_VIA_WRITER));
    return 0;
}

static u32 screen_key;
static int tap_scene(ps2_ctx *ctx, void *u) {
    u32 obj = ps2_arg(ctx, 0);
    (void)u;
    if (obj && obj != ps2_r32(A_SCENE_ROOT_PTR))
        screen_key = ((u32)ps2_r8(obj + 8u) << 8) | ps2_r8(obj + 9u);
    return 0;
}

static u32 last_sent = ~0u;
static int cap_was_on;
static u32 cap_gen = 1;
static u32 defined_gen[RN_EMIT_MAX];

static void send_tag(u32 kind, u32 a, u32 b) {
    if (g_cap_deep) ps2_cap_tag(kind, a, b);
    ps2_gfxq_tag(kind, a, b);
}

static void note_capture(void) {
    if (g_cap_deep && !cap_was_on) {
        cap_gen++;
        last_sent = ~0u;
        ps2_cap_tag(RN_TAG_SCENE, screen_key, 0u);
    }
    cap_was_on = g_cap_deep;
}

static int tap_flush(ps2_ctx *ctx, void *u) {
    u32 dc = ps2_arg(ctx, 0);
    (void)u;
    note_capture();
    send_tag(RN_TAG_SCENE, screen_key, 0u);
    in_flush = 1;
    if (dc) {
        u32 idx = ps2_r8(dc + 363u) & 1u;
        u32 vif = ps2_r32(dc + 340u), gif = ps2_r32(dc + 344u);
        dc_ranges[0].lo = ps2_r32(dc + 324u + 4u * idx);
        dc_ranges[0].hi = vif ? vif + 16u * 64u : 0u;
        dc_ranges[1].lo = ps2_r32(dc + 332u + 4u * idx);
        dc_ranges[1].hi = gif ? gif + 16u * 64u : 0u;
        dc_ranges[2].lo = ps2_r32(dc + 332u + 4u * (1u - idx));
        dc_ranges[2].hi = dc_ranges[2].lo + 16u * 1024u;
        dc_ranges[3].lo = ps2_r32(dc + 324u + 4u * (1u - idx));
        dc_ranges[3].hi = dc_ranges[3].lo + 16u * 64u;
        for (int i = 0; i < 4; i++)
            if (dc_ranges[i].hi <= dc_ranges[i].lo) dc_ranges[i].hi = 0;
    }
    return 0;
}

u32 rn_tap_open_emitter(u32 pkt) {
    u32 best = RN_EMIT_NONE, best_start = 0;
    for (u32 i = 0; i < OPEN_MAX; i++) {
        u32 b = opens[i].buf, base0, base1, size;
        if (!b || opens[i].start > pkt || opens[i].start < best_start) continue;
        base0 = ps2_r32(b);
        base1 = ps2_r32(b + 4u);
        size = ps2_r32(b + 12u);
        if (pkt - base0 >= size && pkt - base1 >= size) continue;
        best = opens[i].emitter;
        best_start = opens[i].start;
    }
    return best;
}

static int tap_flush_after(ps2_ctx *ctx, void *u) {
    (void)ctx; (void)u;
    rn_intent_frame_end();
    in_flush = 0;
    st_frames++;
    if (n_ranges > st_peak_ranges) st_peak_ranges = n_ranges;
    if (ranges_full_said) st_overflow_frames++;
    ranges_full_said = 0;
    n_ranges = 0;
    ranges_sorted = 1;
    memset(dc_ranges, 0, sizeof dc_ranges);
    return 0;
}

void rn_dma_attrib_slow(int ch, u32 tadr) {
    u32 e;
    (void)ch;
    note_capture();
    e = lookup(tadr);
    if (rn_intents_pending) rn_intent_tag(tadr, e);
    if (tadr) {
        st_tags++;
        st_tags_by[e < RN_EMIT_FIRST ? e : RN_EMIT_FIRST]++;
    }
    if (tadr && e == RN_EMIT_NONE && last_sent != ~0u && last_sent != RN_EMIT_NONE) {
        st_inherited++;
        return;
    }
    if (tadr && e == RN_EMIT_NONE && PS2_ENV("PS2_RN_LOG_NOWHERE")) {
        static u32 shown, pages[64];
        u32 pg = tadr >> 12, i;
        for (i = 0; i < shown; i++) if (pages[i] == pg) break;
        if (i == shown && shown < 64u) {
            char nm[160];
            pages[shown++] = pg;
            ps2_log("rn: tag nowhere ch%d @%08X  %08X %08X %08X %08X | "
                    "last emitter %u %s | ranges %u, first %08X..%08X",
                    ch, tadr, ps2_r32(tadr), ps2_r32(tadr + 4u),
                    ps2_r32(tadr + 8u), ps2_r32(tadr + 12u), last_sent,
                    last_sent < RN_EMIT_MAX ? rn_emitter_name(last_sent, nm, sizeof nm) : "",
                    n_ranges, n_ranges ? ranges[0].start : 0u,
                    n_ranges ? ranges[0].end : 0u);
        }
    }
    if (e == last_sent) return;
    if (e >= RN_EMIT_FIRST && g_cap_deep && defined_gen[e] != cap_gen) {
        const rn_emitter *em = rn_emitter_get(e);
        defined_gen[e] = cap_gen;
        if (em) ps2_cap_tag(RN_TAG_DEFINE, e | (em->via << 16), em->site);
    }
    send_tag(RN_TAG_EMITTER, e, 0u);
    last_sent = e;
}

void rn_init(void) {
    const char *e = getenv("PS2_RN_TAPS");
    int bad = 0;
    if (e && *e == '0') {
        ps2_log("rn: render taps off (PS2_RN_TAPS=0)");
        return;
    }
    /* The tables are written in US addresses; find them in whatever executable
     * is actually loaded before anything looks at them. */
    if (!rn_resolve()) return;
    addrs_apply();
    {
        const known_fn *all[] = { &fn_db_open, &fn_db_close, &fn_ot_init,
                                  &fn_ot_open, &fn_ot_close, &fn_ot_link,
                                  &fn_dc_flush };
        for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
            if (all[i]->addr && !code_matches(all[i])) {
                ps2_log("rn: %08X does not match the function the taps expect; "
                        "no render taps", all[i]->addr);
                bad = 1;
            }
        for (size_t i = 0; i < N_2D; i++)
            if (fn_2d[i].addr && !code_matches(&fn_2d[i])) {
                ps2_log("rn: 2D helper %08X does not match; no render taps",
                        fn_2d[i].addr);
                bad = 1;
            }
        for (size_t i = 0; i < N_WRITERS; i++)
            if (writers[i].fn.addr && !code_matches(&writers[i].fn)) {
                ps2_log("rn: packet writer %08X does not match; no render taps",
                        writers[i].fn.addr);
                bad = 1;
            }
    }
    if (bad) return;
    /* Only install the taps whose function was located; the rest are already
     * reported and fall back to the emulated path. */
    if ((RESOLVED(0)  && ps2_hook_before(fn_db_open.addr, tap_db_open, NULL, 100, "rn-taps") < 0)
        || (RESOLVED(1) && ps2_hook_before(fn_db_close.addr, tap_db_close, NULL, 100, "rn-taps") < 0)
        || (RESOLVED(3) && ps2_hook_before(fn_ot_open.addr, tap_ot_open, NULL, 100, "rn-taps") < 0)
        || (RESOLVED(6) && (ps2_hook_before(fn_dc_flush.addr, tap_flush, NULL, 100, "rn-taps") < 0
                            || ps2_hook_after(fn_dc_flush.addr, tap_flush_after, NULL, 100, "rn-taps") < 0))
        || (RESOLVED(16) && ps2_hook_before(scene_dispatch_addr(), tap_scene, NULL, 100, "rn-taps") < 0)) {
        ps2_log("rn: the hook layer refused a tap; render taps are off");
        return;
    }
    for (uintptr_t i = 0; i < N_2D; i++) {
        if (!(resolved_mask & (1u << (7 + i)))) continue;
        if (ps2_hook_before(fn_2d[i].addr, tap_2d, (void *)i, 100, "rn-taps") < 0) {
            ps2_log("rn: the hook layer refused a 2D tap; render taps are off");
            return;
        }
    }
    for (uintptr_t i = 0; i < N_WRITERS; i++) {
        if (!(resolved_mask & (1u << (13 + i)))) continue;
        if (ps2_hook_after(writers[i].fn.addr, tap_writer, (void *)i, 100,
                           "rn-taps") < 0) {
            ps2_log("rn: the hook layer refused a writer tap; render taps are off");
            return;
        }
    }
    rn_taps_on = 1;
    rn_census_on = 1;
    {   const char *c = getenv("PS2_RN_CENSUS");
        if (c && *c == '0') rn_census_on = 0; }
    ps2_log("rn: render taps attached (packet buffer, ordering table, 2D helpers, "
            "DrawCtrl flush); provenance %s", rn_census_on ? "and census on"
                                                          : "on, census off");
}

void rn_tap_report(void) {
    if (!rn_taps_on) return;
    ps2_log("rn: %llu frames, %llu packet ranges (%.1f per frame, peak %u), "
            "%.1f KB per frame; %llu frames overflowed the range table",
            (unsigned long long)st_frames, (unsigned long long)st_ranges,
            st_frames ? (double)st_ranges / (double)st_frames : 0.0,
            st_peak_ranges,
            st_frames ? (double)st_bytes / 1024.0 / (double)st_frames : 0.0,
            (unsigned long long)st_overflow_frames);
    ps2_log("rn: %llu data-carrying DMA tags: %llu in an emitter's range, "
            "%llu DrawCtrl, %llu bucket heads, %llu unowned buffer, "
            "%llu outside the frame, %llu in no range (%llu of them inherited "
            "from the range that called into them)",
            (unsigned long long)st_tags,
            (unsigned long long)st_tags_by[RN_EMIT_FIRST],
            (unsigned long long)st_tags_by[RN_EMIT_DRAWCTRL],
            (unsigned long long)st_tags_by[RN_EMIT_OTHEAD],
            (unsigned long long)st_tags_by[RN_EMIT_UNOWNED],
            (unsigned long long)st_tags_by[RN_EMIT_OFFCHAIN],
            (unsigned long long)st_tags_by[RN_EMIT_NONE],
            (unsigned long long)st_inherited);
}
