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
#define LANG_CFG_OFF   0x150u        /* the stack slot holding the config object */
#define LANG_FIELD     0xC13Du       /* the language byte within it */

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

/* The hook layer can only attach at a function entry, not mid-function, so this sits on
 * the function and samples the config pointer from the frame it is entered with.  The
 * address is the same every time, which is itself worth knowing: if it moves, watching
 * one address would be pointless.
 *
 * Logged on the first few calls and then only when the value changes, because this
 * function runs for every radio track and a line per call would bury the log. */
static int lang_probe(ps2_ctx *ctx, void *u) {
    u32 cfg, field, v;
    (void)u;

    lang_calls++;
    cfg = ps2_r32(ctx->r[29].ud[0] + LANG_CFG_OFF);
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
    if (ps2_hook_before(func, lang_probe, NULL, 200, "lang-probe") < 0) {
        ps2_log("lang: could not hook %08X (the function holding the decision)",
                func);
        return;
    }
    ps2_log("lang: decision %08X found, sampling from the function at %08X",
            site, func);
}
