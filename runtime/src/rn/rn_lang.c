/* Probe the game's choice of radio audio language.
 *
 * The radio language is not stored anywhere the runtime knows about: the game keeps it
 * in a configuration object on the heap and reads one byte of it when it decides which
 * disc file to open.  In the Japanese executable that decision is at 0x00157EE0:
 *
 *     lw   t2, 0x150(sp)          ; the config object
 *     lui  v1, 0x000A
 *     addu v1, v1, t2
 *     lbu  v1, 0xC13D(v1)         ; the language byte
 *     bne  v1, 1, other           ; 1 -> RADIOJE.PAC, otherwise RADIOJJ.PAC
 *
 * Whether the menu's choice reaches that byte is the whole question, and it cannot be
 * answered from the outside: the log shows which file was opened, but not the value
 * that chose it.  This prints both the address and the value, so a single run with
 * PS2_LANG_PROBE=1 says whether the menu wrote the setting and whether this code read
 * it back.
 *
 * The address is computed the same way the game computes it, from the stack pointer at
 * that exact instruction, rather than from a hardcoded guess -- which is also why this
 * only works on the Japanese build: 0x00157EE0 is its address, and the reference build
 * puts this elsewhere.  The probe therefore checks that the code at that address is
 * what it expects before trusting anything it reads.
 */

#include "rn_int.h"
#include "ps2_hook.h"

#include <stdlib.h>

#define LANG_SITE_JP   0x00157EE0u   /* lw t2, 0x150(sp) in the Japanese executable */
#define LANG_FUNC_JP   0x00156D78u   /* the function containing it */
/* The other decision, which picks between the two radio files by a quality flag rather
 * than by language.  Both are worth watching, because which one runs depends on how the
 * track was requested, and the log cannot tell them apart from the file name alone. */
#define QUAL_SITE_JP   0x0015A8FCu   /* lw v1, 0xC09C(cfg) */
#define QUAL_FUNC_JP   0x00158FB0u   /* the function containing it (same frame) */
#define LANG_FRAME     0x1D0u        /* what that function's prologue subtracts */
#define LANG_CFG_OFF   0x150u        /* the stack slot holding the config object */
#define LANG_FIELD     0xC13Du       /* the language byte within it */

/* A "before" hook runs before the function's own prologue, so the frame the game will
 * use does not exist yet: sp still holds the caller's value.  The slot the game reads as
 * 0x150(sp) after its prologue therefore sits at 0x150 - 0x1D0 from the sp seen here.
 * Getting this wrong reads unrelated stack and silently finds nothing to report. */
#define LANG_CFG_FROM_ENTRY_SP ((int)LANG_CFG_OFF - (int)LANG_FRAME)

/* The four instructions the probe depends on, as the Japanese build encodes them.  This
 * is what proves the address is the decision and not merely somewhere in .text. */
static const u32 lang_sig[4] = {
    0x8FAA0150u,   /* lw   t2, 0x150(sp)   */
    0x3C03000Au,   /* lui  v1, 0x000A      */
    0x006A1821u,   /* addu v1, v1, t2      */
    0x9063C13Du,   /* lbu  v1, -0x3EC3(v1) */
};

static int lang_probed;
static unsigned lang_calls;
static u32 lang_last;
static int lang_have_last;
static int lang_said;

/* The hook layer can only attach at a function entry, not mid-function, so this sits on
 * the function and samples the config pointer from the frame it is entered with.  The
 * address is the same every time, which is itself worth knowing: if it moves, watching
 * one address would be pointless.
 *
 * Logged on the first few calls and then only when the value changes, because this
 * function runs for every radio track and a line per call would bury the log. */
static int lang_probe(ps2_ctx *ctx, void *u) {
    u32 cfg, field, v, slot;
    (void)u;

    lang_calls++;
    slot = (u32)((s32)ctx->r[29].ud[0] + LANG_CFG_FROM_ENTRY_SP);
    cfg = ps2_r32(slot);
    /* Report the raw slot too: if the offset is wrong this is what shows it, and a
     * probe that only prints when it is right says nothing when it is wrong. */
    if (!lang_said || lang_calls <= 3u)
        ps2_log("lang: call %u  sp %08X  slot %08X -> cfg %08X",
                lang_calls, ctx->r[29].ud[0], slot, cfg);
    lang_said = 1;
    if (!cfg || cfg >= 0x02000000u) return 0;
    field = cfg + LANG_FIELD;
    v = ps2_r8(field);
    if (lang_have_last && v == lang_last && lang_calls > 4u) return 0;
    lang_last = v;
    lang_have_last = 1;
    ps2_log("lang: call %u  config %08X  language byte %08X = %u  -> %s",
            lang_calls, cfg, field, v,
            v == 1u ? "RADIOJE.PAC (Japanese)" : "RADIOJJ.PAC (English)");
    return 0;
}

/* The second decision, which picks between the two radio files by a quality bit rather
 * than by language.  It reaches the same two filenames by a different path, so whichever
 * one runs has to be visible: from the filename alone they cannot be told apart.
 *
 * Reported unconditionally for the first few calls, so that a wrong offset shows up as
 * an implausible number instead of as silence. */
static int qual_probe(ps2_ctx *ctx, void *u) {
    u32 cfg, slot, v;
    static unsigned n;
    (void)u;
    n++;
    if (n > 4u) return 0;
    slot = (u32)((s32)ctx->r[29].ud[0] + LANG_CFG_FROM_ENTRY_SP);
    cfg = ps2_r32(slot);
    if (!cfg || cfg >= 0x02000000u) {
        ps2_log("qual: call %u  slot %08X -> cfg %08X (not a pointer)", n, slot, cfg);
        return 0;
    }
    v = ps2_r32(cfg + 0xC09Cu);
    ps2_log("qual: call %u  config %08X  0xC09C = %08X  bit20=%u -> %s",
            n, cfg, v, (v >> 20) & 1u,
            (v & 0x00100000u) ? "RADIOJJ.PAC (larger)" : "RADIOJE.PAC");
    return 0;
}

/* The routine every radio-selection site calls, with the chosen filename in a1.  Hooking
 * it shows the decision that was actually used, rather than the one a particular site
 * would have made -- which is the difference that mattered here: the language byte read
 * 0 (English) at 0x00157EE0 while RADIOJJ.PAC was what got loaded, so the file was picked
 * somewhere else. */
#define LANG_LOADER_JP 0x0038171Cu

static int loader_probe(ps2_ctx *ctx, void *u) {
    u32 p = ctx->r[5].ud[0];          /* a1 */
    static unsigned n;
    char s[40];
    int i;
    (void)u;
    n++;
    if (n > 20u) return 0;
    if (p < 0x00100000u || p >= 0x02000000u) {
        ps2_log("load: call %u  a1 = %08X (not a pointer)", n, p);
        return 0;
    }
    for (i = 0; i < 39; i++) {
        u8 c = ps2_r8(p + (u32)i);
        if (!c) break;
        s[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    s[i] = 0;
    ps2_log("load: call %u  a1 = %08X  \"%s\"", n, p, s);
    return 0;
}

void rn_lang_probe_init(void) {
    const char *e = getenv("PS2_LANG_PROBE");
    u32 site, func;
    int i;

    if (lang_probed) return;
    lang_probed = 1;
    if (!e || *e == '0') return;

    site = rn_resolve_addr(LANG_SITE_JP);
    for (i = 0; i < 4; i++)
        if (ps2_image_word(site + 4u * (u32)i) != lang_sig[i]) {
            ps2_log("lang: the language decision is not at %08X on this build; "
                    "probe disabled", site);
            return;
        }
    func = rn_resolve_addr(LANG_FUNC_JP);
    if (ps2_hook_before(func, lang_probe, NULL, 200, "lang-probe") < 0)
        ps2_log("lang: could not hook %08X", func);
    {
        u32 qual = rn_resolve_addr(QUAL_FUNC_JP);
        if (qual != func && ps2_hook_before(qual, qual_probe, NULL, 200,
                                           "lang-probe") < 0)
            ps2_log("lang: could not hook %08X", qual);
    }
    {
        u32 load = rn_resolve_addr(LANG_LOADER_JP);
        if (ps2_hook_before(load, loader_probe, NULL, 200, "lang-probe") < 0)
            ps2_log("lang: could not hook the loader at %08X", load);
    }
}
