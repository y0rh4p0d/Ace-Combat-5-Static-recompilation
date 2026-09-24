#include "ps2_runtime.h"
#include "ps2_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define HOOK_MAX_CHAIN   8
#define STUB_BYTES       16
/* An AArch64 stub needs a literal pool entry for the thunk address, so it takes
 * STUB_BYTES + 8.  Size the pool with the larger of the two so the count is
 * right on both architectures. */
#define STUB_STRIDE      (STUB_BYTES + 8)
#define STUB_POOL_BYTES  (64u * 1024u)
#define HOOK_MAX_SITES   (STUB_POOL_BYTES / STUB_STRIDE)

typedef struct {
    ps2_hook_fn fn;
    void *user;
    int priority;
    int handle;
    const char *owner;
} hook_entry;

typedef struct {
    u32 guest;
    u8 *host;
    u8 *stub;
    int patched;
    hook_entry before[HOOK_MAX_CHAIN], after[HOOK_MAX_CHAIN];
    unsigned nbefore, nafter;
    ps2_hook_fn replacement;
    void *replacement_user;
    int replacement_priority, replacement_handle;
    const char *replacement_owner;
    u64 calls, declined, after_skipped;
} hook_site;

static hook_site *sites;
static unsigned nsites;
static u8 *stub_pool;
static unsigned stub_used;
static int hook_state;

typedef struct {
    u32 from, to;
    ps2_fn original;
    int priority, handle;
    const char *owner;
} redirect;
static redirect *redirects;
static unsigned nredirects, credirects;

static int next_handle = 1;

/* Entry detours.
 *
 * A patched function jumps to a stub, which loads a site id and branches to the
 * thunk below.  The thunk calls ps2_hook_enter(id, ctx); that returns either the
 * address of the untouched function body (so the thunk jumps there, and the
 * guest runs its own code after the hooks have seen the call) or NULL when a
 * hook replaced the function outright, in which case the thunk returns to the
 * caller.
 *
 * The thunk has to preserve the one argument the recompiled functions take, and
 * it must not disturb the guest's stack layout, because the code it jumps to
 * expects to have been entered directly.
 *
 * Non-volatile registers carry the guest's state across a call: x19..x28 are
 * callee-saved, so calling ps2_hook_enter from the thunk is safe, but the
 * context pointer in x0 is volatile and has to be preserved across the call.
 * The id is smuggled in x10/w10, which is a caller-saved scratch register that
 * is not used to pass arguments, so the stub does not have to touch the stack.
 */

#if defined(_WIN32) && defined(__aarch64__)
/* x10 = site id, x0 = ps2_ctx *. */
__asm__(
    ".text\n"
    ".globl ps2_hook_thunk\n"
    "ps2_hook_thunk:\n"
    "    stp  x29, x30, [sp, #-16]!\n"   /* keep the stack 16-byte aligned */
    "    mov  x29, sp\n"
    "    mov  w1, w10\n"                 /* arg1 = id                    */
    "    bl   ps2_hook_enter\n"          /* arg0 = ctx (already in x0)   */
    "    ldp  x29, x30, [sp], #16\n"
    "    cbz  x0, 1f\n"
    "    br   x0\n"                      /* run the original body        */
    "1:  ret\n");
void ps2_hook_thunk(void);
#define HOOK_SUPPORTED 1
#define HOOK_ID_REG "w10"
#elif defined(_WIN32) && defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl ps2_hook_thunk\n"
    "ps2_hook_thunk:\n"
    "    pushq %rcx\n"
    "    subq  $0x20, %rsp\n"
    "    movq  %rcx, %rdx\n"
    "    movl  %r10d, %ecx\n"
    "    call  ps2_hook_enter\n"
    "    addq  $0x20, %rsp\n"
    "    popq  %rcx\n"
    "    testq %rax, %rax\n"
    "    jz    1f\n"
    "    jmpq  *%rax\n"
    "1:  ret\n");
void ps2_hook_thunk(void);
#define HOOK_SUPPORTED 1
#else
#define HOOK_SUPPORTED 0
#endif

typedef struct hook_frame {
    const struct hook_frame *prev;
    u32 args[8];
    u32 sp;
} hook_frame;

static __thread const hook_frame *cur_frame;
static __thread unsigned call_depth;

void *ps2_hook_enter(u32 id, ps2_ctx *ctx);
void *ps2_hook_enter(u32 id, ps2_ctx *ctx) {
    hook_site *s = &sites[id];
    hook_entry chain[HOOK_MAX_CHAIN];
    unsigned n = s->nbefore;
    ps2_hook_fn rep;
    void *rep_user;
    s->calls++;
    if (n) {
        memcpy(chain, s->before, n * sizeof *chain);
        for (unsigned i = 0; i < n; i++) chain[i].fn(ctx, chain[i].user);
    }
    rep = s->replacement;
    rep_user = s->replacement_user;
    n = s->nafter;
    if (!n || call_depth >= HOOK_MAX_CALL_DEPTH) {
        if (n) s->after_skipped++;
        if (rep) {
            if (rep(ctx, rep_user)) return NULL;
            s->declined++;
        }
        return s->host + 8;
    }
    {
        hook_frame fr;
        memcpy(chain, s->after, n * sizeof *chain);
        for (int i = 0; i < 8; i++)
            fr.args[i] = ctx->r[i < 4 ? 4 + i : 8 + (i - 4)].uw[0];
        fr.sp = ctx->r[29].uw[0];
        fr.prev = cur_frame;
        call_depth++;
        if (!rep || !rep(ctx, rep_user)) {
            if (rep) s->declined++;
            (void)((ps2_fn)(void *)(s->host + 8))(ctx);
        }
        cur_frame = &fr;
        for (unsigned i = 0; i < n; i++) chain[i].fn(ctx, chain[i].user);
        cur_frame = fr.prev;
        call_depth--;
    }
    return NULL;
}

u32 ps2_hook_entry_arg(int n, int *ok) {
    const hook_frame *f = cur_frame;
    if (!f || n < 0) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    if (n < 8) return f->args[n];
    return ps2_r32(f->sp + 4u * (u32)(n - 8));
}

static int table_sorted = -1;

static u8 *host_of(u32 guest) {
    if (table_sorted < 0) {
        table_sorted = 1;
        for (unsigned i = 1; i < ps2_func_count; i++)
            if (ps2_func_table[i - 1].addr >= ps2_func_table[i].addr) {
                table_sorted = 0;
                break;
            }
    }
    if (table_sorted) {
        unsigned lo = 0, hi = ps2_func_count;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2u;
            if (ps2_func_table[mid].addr == guest)
                return (u8 *)(void *)ps2_func_table[mid].fn;
            if (ps2_func_table[mid].addr < guest) lo = mid + 1u;
            else hi = mid;
        }
        return NULL;
    }
    for (unsigned i = 0; i < ps2_func_count; i++)
        if (ps2_func_table[i].addr == guest)
            return (u8 *)(void *)ps2_func_table[i].fn;
    return NULL;
}

static hook_site *site_of(u32 guest) {
    for (unsigned i = 0; i < nsites; i++)
        if (sites[i].guest == guest) return &sites[i];
    return NULL;
}

static const char *who(const char *owner) { return owner ? owner : "?"; }

static int may_patch(const char *what, u32 addr, const char *owner) {
    if (ps2_kernel_on_ee_thread()) return 1;
    ps2_log("hook: %s cannot %s %08X from this thread -- attach from a hook, "
            "a field callback or at load, where the guest is not running "
            "under you", who(owner), what, addr);
    return 0;
}

#if HOOK_SUPPORTED
static u8 *alloc_near(void *anchor, unsigned bytes) {
    SYSTEM_INFO si;
    uintptr_t base, gran;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    base = (uintptr_t)anchor & ~(gran - 1u);
    for (uintptr_t d = gran; d < 0x40000000ull; d += gran) {
        void *p;
        p = VirtualAlloc((void *)(base + d), bytes, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE);
        if (p) return (u8 *)p;
        if (base > d) {
            p = VirtualAlloc((void *)(base - d), bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
            if (p) return (u8 *)p;
        }
    }
    return NULL;
}
#endif

int ps2_hook_init(void) {
#if !HOOK_SUPPORTED
    if (!hook_state) {
        hook_state = -1;
        ps2_log("hook: entry detours are implemented for 64-bit Windows "
                "x86_64 and ARM64 only");
    }
    return -1;
#else
    if (hook_state) return hook_state > 0 ? 0 : -1;
    hook_state = -1;
    if (!ps2_func_count) {
        ps2_log("hook: there are no recompiled functions to hook");
        return -1;
    }
    sites = (hook_site *)calloc(HOOK_MAX_SITES, sizeof *sites);
    stub_pool = alloc_near((void *)ps2_func_table[0].fn, STUB_POOL_BYTES);
    if (!sites || !stub_pool) {
        ps2_log("hook: no executable memory within reach of the code; "
                "detours are unavailable");
        return -1;
    }
    hook_state = 1;
    return 0;
#endif
}

#if HOOK_SUPPORTED

#if defined(__aarch64__)

/* AArch64 encodings used to splice the detour in. */
static u32 a64_movz_w(int rd, u32 imm16) {          /* movz Wd, #imm16      */
    return 0x52800000u | ((imm16 & 0xFFFFu) << 5) | (u32)(rd & 31);
}
static u32 a64_movk_w(int rd, u32 imm16, int shift) { /* movk Wd, #imm16, lsl n */
    return 0x72800000u | ((u32)(shift / 16) << 21)
           | ((imm16 & 0xFFFFu) << 5) | (u32)(rd & 31);
}
static u32 a64_ldr_imm(int rt, int rn, int off) {   /* ldr Xt, [Xn, #off]   */
    return 0xF9400000u | ((u32)(off / 8) << 10) | ((u32)(rn & 31) << 5)
           | (u32)(rt & 31);
}
static u32 a64_br(int rn) { return 0xD61F0000u | ((u32)(rn & 31) << 5); }
static u32 a64_b(long long from, long long to) {    /* b  <to>              */
    long long d = to - from;
    return 0x14000000u | (u32)((d >> 2) & 0x03FFFFFFll);
}
static int a64_b_reachable(long long from, long long to) {
    long long d = to - from;
    return d >= -0x8000000ll && d <= 0x7FFFFFCll;
}

/* stub (16 bytes):  movz/movk id -> x10 ; ldr x16,[pc,#8] ; br x16 ; <thunk> */
static int write_stub_a64(u8 *stub, unsigned id, void *thunk) {
    u64 target = (u64)(uintptr_t)thunk;
    memcpy(stub + 0, &(u32){ a64_movz_w(10, id & 0xFFFFu) }, 4);
    memcpy(stub + 4, &(u32){ a64_movk_w(10, (id >> 16) & 0xFFFFu, 16) }, 4);
    memcpy(stub + 8, &(u32){ a64_ldr_imm(16, 31, 8) }, 4);   /* loads stub+16 */
    memcpy(stub + 12, &(u32){ a64_br(16) }, 4);
    memcpy(stub + 16, &target, 8);
    return 1;
}
#else
/* Not AArch64: only the x86_64 generate path below is compiled, and it does not
 * call this. */
static int write_stub_a64(u8 *stub, unsigned id, void *thunk) {
    (void)stub; (void)id; (void)thunk;
    return 0;
}
#endif  /* __aarch64__ */

/* Entry detour, per architecture. */
#if defined(__aarch64__)
static int write_detour(hook_site *s) {
    DWORD old;
    unsigned id = (unsigned)(s - sites);
    u8 *stub = stub_pool + stub_used;
    long long to_stub;

    if (s->patched) return 0;
    /* The sled is 8 NOPs; the detour goes in the first instruction and the rest
     * is left as padding.  NOP is 0xD503201F on AArch64, not 0x90. */
    for (int i = 0; i < 8; i++) {
        u32 w;
        memcpy(&w, s->host + 4 * i, 4);
        if (w == 0xD503201Fu) continue;
        ps2_log("hook: %08X does not begin with a patch sled -- this exe was "
                "built without PS2_HOOK_ENTRIES, so entry hooks are refused",
                s->guest);
        return -1;
    }
    /* The stub must reach the patched entry with a single 26-bit branch, so
     * allow only the sled's own span and skip the next function. */
    to_stub = (long long)(stub - s->host);
    if (!a64_b_reachable((long long)(s->host + 0), (long long)stub)) {
        ps2_log("hook: %08X is out of a 26-bit branch's reach of the stub area",
                s->guest);
        return -1;
    }
    if (!write_stub_a64(stub, id, (void *)ps2_hook_thunk)) return -1;
    FlushInstructionCache(GetCurrentProcess(), stub, STUB_BYTES + 8);
    if (!VirtualProtect(s->host, 8, PAGE_EXECUTE_READWRITE, &old)) {
        ps2_log("hook: cannot make %08X writable (%lu)", s->guest,
                (unsigned long)GetLastError());
        return -1;
    }
    {
        u32 b = a64_b((long long)s->host, (long long)stub);
        memcpy(s->host, &b, 4);
    }
    VirtualProtect(s->host, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s->host, 8);
    stub_used += STUB_BYTES + 8;
    s->stub = stub;
    s->patched = 1;
    return 0;
}
#else
static int write_detour(hook_site *s) {
    DWORD old;
    unsigned id = (unsigned)(s - sites);
    u8 *stub = stub_pool + stub_used;
    long long to_thunk, to_stub;

    if (s->patched) return 0;
    for (int i = 0; i < 8; i++) {
        if (s->host[i] == 0x90) continue;
        ps2_log("hook: %08X does not begin with a patch sled -- this exe was "
                "built with PS2_HOOK_ENTRIES=OFF, so hooks are refused",
                s->guest);
        return -1;
    }
    to_thunk = (long long)((u8 *)(void *)ps2_hook_thunk - (stub + 11));
    to_stub = (long long)(stub - (s->host + 5));
    if (to_thunk > 0x7FFFFFFFll || to_thunk < -0x80000000ll
        || to_stub > 0x7FFFFFFFll || to_stub < -0x80000000ll) {
        ps2_log("hook: %08X is out of a 32-bit jump's reach of the stub area",
                s->guest);
        return -1;
    }
    stub[0] = 0x41; stub[1] = 0xBA;
    memcpy(stub + 2, &id, 4);
    stub[6] = 0xE9;
    { int32_t r32 = (int32_t)to_thunk; memcpy(stub + 7, &r32, 4); }
    FlushInstructionCache(GetCurrentProcess(), stub, STUB_BYTES);
    if (!VirtualProtect(s->host, 8, PAGE_EXECUTE_READWRITE, &old)) {
        ps2_log("hook: cannot make %08X writable (%lu)", s->guest,
                (unsigned long)GetLastError());
        return -1;
    }
    s->host[0] = 0xE9;
    { int32_t r32 = (int32_t)to_stub; memcpy(s->host + 1, &r32, 4); }
    VirtualProtect(s->host, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s->host, 8);
    stub_used += STUB_BYTES;
    s->stub = stub;
    s->patched = 1;
    return 0;
}
#endif  /* __aarch64__ */
#else   /* !HOOK_SUPPORTED */
static int write_detour(hook_site *s) { (void)s; return -1; }
#endif
static hook_site *prepare(u32 guest_addr, const char *what, const char *owner) {
    hook_site *s;
    u8 *host;
    if (!may_patch(what, guest_addr, owner)) return NULL;
    if (ps2_hook_init() != 0) return NULL;
    host = host_of(guest_addr);
    if (!host) {
        ps2_log("hook: %08X is not a recompiled function (%s)", guest_addr,
                who(owner));
        return NULL;
    }
    s = site_of(guest_addr);
    if (!s) {
        if (nsites == HOOK_MAX_SITES) {
            ps2_log("hook: %u functions are hooked already, which is all the "
                    "stub area holds; %s cannot hook %08X", HOOK_MAX_SITES,
                    who(owner), guest_addr);
            return NULL;
        }
        s = &sites[nsites];
        memset(s, 0, sizeof *s);
        s->guest = guest_addr;
        s->host = host;
        if (write_detour(s) != 0) return NULL;
        nsites++;
    }
    return s;
}

static int chain_insert(hook_entry *chain, unsigned *n, u32 addr,
                        ps2_hook_fn fn, void *user, int priority,
                        const char *owner, const char *kind) {
    unsigned at;
    if (*n == HOOK_MAX_CHAIN) {
        ps2_log("hook: %08X already has %d %s-hooks on it; %s's is refused",
                addr, HOOK_MAX_CHAIN, kind, who(owner));
        return -1;
    }
    for (at = 0; at < *n; at++)
        if (chain[at].priority < priority) break;
    memmove(&chain[at + 1], &chain[at], (*n - at) * sizeof chain[0]);
    chain[at].fn = fn;
    chain[at].user = user;
    chain[at].priority = priority;
    chain[at].owner = owner;
    chain[at].handle = next_handle++;
    (*n)++;
    {
        const char *nm = ps2_symbol_name(addr);
        ps2_log("hook: %s runs %s %08X%s%s (priority %d)", who(owner), kind,
                addr, nm ? " " : "", nm ? nm : "", priority);
    }
    return chain[at].handle;
}

int ps2_hook_before(u32 guest_addr, ps2_hook_fn fn, void *user,
                    int priority, const char *owner) {
    hook_site *s;
    if (!fn || !(s = prepare(guest_addr, "hook", owner))) return -1;
    return chain_insert(s->before, &s->nbefore, guest_addr, fn, user, priority,
                        owner, "before");
}

int ps2_hook_after(u32 guest_addr, ps2_hook_fn fn, void *user,
                   int priority, const char *owner) {
    hook_site *s;
    if (!fn || !(s = prepare(guest_addr, "hook", owner))) return -1;
    return chain_insert(s->after, &s->nafter, guest_addr, fn, user, priority,
                        owner, "after");
}

int ps2_hook_replace(u32 guest_addr, ps2_hook_fn fn, void *user, int priority,
                     const char *owner) {
    hook_site *s;
    const char *nm = ps2_symbol_name(guest_addr);
    if (!fn || !(s = prepare(guest_addr, "replace", owner))) return -1;
    if (s->replacement) {
        if (s->replacement_priority >= priority) {
            ps2_log("hook: conflict on %08X -- %s (priority %d) already "
                    "replaces it, so %s (priority %d) cannot", guest_addr,
                    who(s->replacement_owner), s->replacement_priority,
                    who(owner), priority);
            return -1;
        }
        ps2_log("hook: conflict on %08X -- %s (priority %d) takes it over "
                "from %s (priority %d)", guest_addr, who(owner), priority,
                who(s->replacement_owner), s->replacement_priority);
    }
    s->replacement_user = user;
    s->replacement_priority = priority;
    s->replacement_owner = owner;
    s->replacement_handle = next_handle++;
    s->replacement = fn;
    ps2_log("hook: %s replaces %08X%s%s (priority %d)", who(owner), guest_addr,
            nm ? " " : "", nm ? nm : "", priority);
    return s->replacement_handle;
}

void ps2_hook_call_original(u32 guest_addr, ps2_ctx *ctx) {
    hook_site *s = site_of(guest_addr);
    u8 *host = s ? s->host : host_of(guest_addr);
    if (!host) {
        ps2_log("hook: call_original on %08X, which is not a recompiled "
                "function", guest_addr);
        return;
    }
    ((ps2_fn)(void *)(s && s->patched ? host + 8 : host))(ctx);
}

int ps2_hook_redirect(u32 from_guest, u32 to_guest, int priority,
                      const char *owner) {
    u8 *target = host_of(to_guest);
    u32 lo, hi;
    redirect *r = NULL;
    ps2_fn original;
    if (!may_patch("redirect", from_guest, owner)) return -1;
    ps2_text_bounds(&lo, &hi);
    if ((from_guest & 3u) || from_guest < lo || from_guest >= hi) {
        ps2_log("hook: %08X is not an instruction address in .text, so %s "
                "cannot redirect it", from_guest, who(owner));
        return -1;
    }
    if (!target) {
        ps2_log("hook: %08X is not a recompiled function, so %s cannot "
                "redirect to it", to_guest, who(owner));
        return -1;
    }
    for (unsigned i = 0; i < nredirects; i++)
        if (redirects[i].from == from_guest) r = &redirects[i];
    if (r) {
        if (r->priority >= priority) {
            ps2_log("hook: conflict on indirect calls to %08X -- %s (priority "
                    "%d) already sends them to %08X, so %s (priority %d) "
                    "cannot", from_guest, who(r->owner), r->priority, r->to,
                    who(owner), priority);
            return -1;
        }
        ps2_log("hook: conflict on indirect calls to %08X -- %s (priority %d) "
                "takes them over from %s (priority %d)", from_guest,
                who(owner), priority, who(r->owner), r->priority);
        original = r->original;
    } else {
        original = ps2_dispatch_lookup(from_guest);
        if (nredirects == credirects) {
            redirect *grown;
            credirects = credirects ? credirects * 2u : 16u;
            grown = (redirect *)realloc(redirects, credirects * sizeof *redirects);
            if (!grown) ps2_fatal("hook: out of memory recording a redirect");
            redirects = grown;
        }
        r = &redirects[nredirects++];
    }
    if (ps2_dispatch_redirect(from_guest, (ps2_fn)(void *)target) != 0) {
        ps2_log("hook: %08X is outside the dispatch table (%s)", from_guest,
                who(owner));
        return -1;
    }
    r->from = from_guest;
    r->to = to_guest;
    r->original = original;
    r->priority = priority;
    r->owner = owner;
    r->handle = next_handle++;
    ps2_log("hook: %s sends indirect calls for %08X to %08X (priority %d)",
            who(owner), from_guest, to_guest, priority);
    return r->handle;
}

static int chain_remove(hook_entry *chain, unsigned *n, int handle, u32 addr,
                        const char *kind) {
    for (unsigned i = 0; i < *n; i++) {
        if (chain[i].handle != handle) continue;
        ps2_log("hook: %s's %s-hook on %08X removed", who(chain[i].owner),
                kind, addr);
        memmove(&chain[i], &chain[i + 1], (*n - i - 1) * sizeof chain[0]);
        (*n)--;
        return 0;
    }
    return -1;
}

int ps2_hook_remove(int handle) {
    if (handle <= 0) return -1;
    if (!ps2_kernel_on_ee_thread()) {
        ps2_log("hook: a hook can only be removed from a thread that holds "
                "the EE");
        return -1;
    }
    for (unsigned i = 0; i < nsites; i++) {
        hook_site *s = &sites[i];
        if (chain_remove(s->before, &s->nbefore, handle, s->guest, "before") == 0
            || chain_remove(s->after, &s->nafter, handle, s->guest, "after") == 0)
            return 0;
        if (s->replacement && s->replacement_handle == handle) {
            ps2_log("hook: %s no longer replaces %08X",
                    who(s->replacement_owner), s->guest);
            s->replacement = NULL;
            s->replacement_user = NULL;
            s->replacement_owner = NULL;
            s->replacement_handle = 0;
            return 0;
        }
    }
    for (unsigned i = 0; i < nredirects; i++) {
        redirect *r = &redirects[i];
        if (r->handle != handle) continue;
        ps2_dispatch_redirect(r->from, r->original);
        ps2_log("hook: %s's redirect of %08X removed", who(r->owner), r->from);
        *r = redirects[--nredirects];
        return 0;
    }
    return -1;
}

static u64 selftest_calls;

static int selftest_hook(ps2_ctx *ctx, void *user) {
    (void)ctx;
    (void)user;
    selftest_calls++;
    return 0;
}

void ps2_hook_selftest_attach(void);
void ps2_hook_selftest_attach(void) {
    if (ps2_hook_before(0x0011DB30u, selftest_hook, NULL, 0,
                        "hook-selftest") > 0)
        ps2_log("hook: self-test attached to 0011DB30");
}

static void report_chain(const hook_entry *chain, unsigned n, const char *kind) {
    for (unsigned k = 0; k < n; k++)
        ps2_log("      %-7s %s (priority %d)", kind, who(chain[k].owner),
                chain[k].priority);
}

void ps2_hook_report(void) {
    if (selftest_calls)
        ps2_log("hook: self-test saw %llu call(s) of 0011DB30",
                (unsigned long long)selftest_calls);
    if (!nsites && !nredirects) return;
    ps2_log("---- hooks ----");
    for (unsigned i = 0; i < nsites; i++) {
        const hook_site *s = &sites[i];
        const char *nm = ps2_symbol_name(s->guest);
        ps2_log("   %08X %-24s %llu call(s)%s", s->guest, nm ? nm : "",
                (unsigned long long)s->calls,
                s->replacement ? "  [body replaced]" : "");
        report_chain(s->before, s->nbefore, "before:");
        if (s->replacement)
            ps2_log("      body:   %s (priority %d), declined %llu time(s)",
                    who(s->replacement_owner), s->replacement_priority,
                    (unsigned long long)s->declined);
        report_chain(s->after, s->nafter, "after:");
        if (s->after_skipped)
            ps2_log("      after-hooks skipped on %llu call(s) nested more "
                    "than %d deep (a guest loop running through this "
                    "function by tail jumps)",
                    (unsigned long long)s->after_skipped, HOOK_MAX_CALL_DEPTH);
    }
    for (unsigned i = 0; i < nredirects; i++)
        ps2_log("   indirect %08X -> %08X  %s (priority %d)", redirects[i].from,
                redirects[i].to, who(redirects[i].owner), redirects[i].priority);
}
