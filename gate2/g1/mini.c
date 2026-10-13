// Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
        /* mini.c - the B-1 bootstrap SUBSTRATE for compiled MountX.
 *
 * This is NOT a second interpreter and NOT the language. Per COMPILE_DESIGN §1
 * it is the floor that compiler.mx will stand on: enough of a runtime to hold
 * values, walk a tiny subset, and get the ERROR CHANNEL right - which is what
 * B-1 exists to prove (§10, red line).
 *
 * It executes the 12 cases in b1/corpus/ and nothing else. Every construct
 * outside the measured subset is a hard refusal (mx_unsupported), never a silent
 * no-op. See KICKOFF.md §1 for the subset and §4 for the red line.
 *
 * C99, no dependencies beyond libc (CB-E/CB-F: no sqlite, no curl).
 */

/* POSIX feature selection, BEFORE every include - including the first.
 *
 * getcwd, clock_gettime and nanosleep are POSIX, not ISO C99, and -std=c99
 * hides them (which is correct C: they are not in the standard). Asking for them
 * with one feature macro is a much smaller concession than dropping -std=c99.
 *
 * BEFORE stdio.h specifically, and that is the whole point: glibc's features.h
 * is pulled in by the FIRST include and CACHES what it found. A _POSIX_C_SOURCE
 * defined after that is a redefinition with no effect - which is exactly what
 * happened here. The block sat between stdarg.h and unistd.h for a long time and
 * worked only because nothing yet needed clock_gettime; the first native that did
 * found it had never been enabled at all. The comment above this one is the
 * reason it moved, and it stays because the failure is invisible otherwise. */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define MX_GEN_CHAIN_MAX 64
#endif
#else
#ifndef MX_GEN_CHAIN_MAX
#define MX_GEN_CHAIN_MAX 64
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <setjmp.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <math.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mx_os_getcwd(buf, sz) _getcwd((buf), (sz))
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define mx_os_getcwd(buf, sz) getcwd((buf), (sz))
#endif

/* MX_VERSION - the native's answer, and the ONE place it is written.
 *
 * The host returns interpreter.py's VERSION constant (:23). It is spelled out
 * here rather than extracted, and the divergence is checked rather than
 * assumed: fbtest asserts `mx_version()` equals VERSION, so if this drifts from
 * the host the fbtest comparison fails loudly rather than passing quietly.
 * That is the whole reason the constant is duplicated instead of generated -
 * a golden is the thing that notices. */
#define MX_VERSION "1.1.0"
#include <math.h>

/* Two build modes from one source (B-2 needs the substrate LINKABLE):
 *   default        - the whole interpreter, main() included (B-1's gate)
 *   -DMX_SUBSTRATE - only the substrate, with its entry points exported so a
 *                    generated .c can link against them. The lexer, parser and
 *                    evaluator are excluded: the generated code replaces them.
 */
#ifdef MX_SUBSTRATE
#define MXFN
#else
#define MXFN static
#endif

/* ===================================================================== */
/* 1. Arena allocator - CB-B. No free before exit.                      */
/* ===================================================================== */
/* The coupling that matters, from COMPILE_DESIGN §6.2: because nothing is
 * freed, longjmp has no cleanup to skip, which is what makes CB-G's use of it
 * safe. If allocation ever becomes refcounted (§3.4's turn), THIS FILE's error
 * strategy must be re-opened. */

#define ARENA_CHUNK (2 * 1024 * 1024)

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used, cap;
    char data[1];
} ArenaChunk;

static ArenaChunk *g_arena = NULL;

/* MX_ARENA_MAX_BYTES - the substrate's MEMORY CEILING, and the first of the two
 * safety nets.
 *
 * There was no ceiling: a program that allocated in a loop consumed RAM until
 * the machine froze, and the crown run did exactly that. The arena grows a chunk
 * at a time and never said no, so "out of memory" was the KERNEL saying it, at
 * a moment of the operating system's choosing rather than ours.
 *
 * It counts ALLOCATION, not live bytes - the arena never frees, so the two are
 * the same number here, and counting allocation is the one that stops a runaway
 * loop.
 *
 * The DEFAULT is 2 GB, not the 512 MB first tried, and the measurement is why:
 * every loop iteration allocates an MxEnv that is never freed, measured at
 * roughly 1 KB per iteration, so 512 MB expired after about half a million
 * iterations - which is a perfectly ordinary amount of looping and would have
 * killed a legitimate program. A ceiling below the working set of a real
 * program is not a safety net, it is a time bomb. 2 GB is far above what this
 * tree needs and still low enough to fail long before the machine is in
 * trouble; a machine with less should set MOUNTX_ARENA_MAX lower deliberately.
 *
 * The limit is an environment variable so a test can lower it to something that
 * actually fires in a second, which is the only way to keep the guard itself
 * honest. */
#if defined(__wasm__) || defined(__wasm32__) || defined(__EMSCRIPTEN__)
#define MX_ARENA_MAX_BYTES_DEFAULT (1024ULL * 1024ULL * 1024ULL)
#else
#define MX_ARENA_MAX_BYTES_DEFAULT (4096ULL * 1024ULL * 1024ULL)
#endif
static size_t g_arena_total = 0;
static size_t g_arena_max = MX_ARENA_MAX_BYTES_DEFAULT;

static size_t g_diag_counts[512] = {0};
static size_t g_diag_bytes[512] = {0};
void print_arena_report(void) {
    if (!getenv("MOUNTX_REPORT_ALLOC")) return;
    fprintf(stderr, "=== ARENA ALLOCATION REPORT (Total: %zu bytes) ===\n", g_arena_total);
    for (size_t i = 0; i < 512; i++) {
        if (g_diag_counts[i] > 0) {
            fprintf(stderr, "  Size %4zu B: %8zu allocs (%10zu bytes)\n",
                    i * 16, g_diag_counts[i], g_diag_bytes[i]);
        }
    }
}

MXFN void *arena_alloc(size_t n) {
    static int g_arena_inited = 0;
    if (!g_arena_inited) {
        const char *e = getenv("MOUNTX_ARENA_MAX");
        if (e && *e) {
            long long v = atoll(e);
            if (v > 0) g_arena_max = (size_t)v;
        }
        atexit(print_arena_report);
        g_arena_inited = 1;
    }
    n = (n + 15u) & ~(size_t)15u;           /* 16-byte align */
    if (n > g_arena_max || g_arena_total + n > g_arena_max) {
        fprintf(stderr, "[Runtime] Out of memory: exceeded %lu MB\n",
                (unsigned long)(g_arena_max / (1024u * 1024u)));
        fflush(stderr);
        exit(70);                          /* malloc's own code, reused */
    }
    g_arena_total += n;
    size_t slot = n / 16;
    if (slot < 512) {
        g_diag_counts[slot]++;
        g_diag_bytes[slot] += n;
    }
    if (!g_arena || g_arena->used + n > g_arena->cap) {
        size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        ArenaChunk *c = (ArenaChunk *)malloc(sizeof(ArenaChunk) + cap);
        if (!c) { fprintf(stderr, "out of memory\n"); exit(70); }
        c->next = g_arena; c->used = 0; c->cap = cap;
        g_arena = c;
    }
    {   void *p = g_arena->data + g_arena->used;
        g_arena->used += n;
        return p;
    }
}

/* ===================================================================== */
/* 1.1 String Interning / Symbol Table                                   */
/* ===================================================================== */
#define MX_INTERN_BUCKETS 4096
#define MX_INTERN_MASK    (MX_INTERN_BUCKETS - 1)

typedef struct MxInternEntry {
    const char *str;
    size_t len;
    uint32_t hash;
    struct MxInternEntry *next;
} MxInternEntry;

static MxInternEntry *g_intern_table[MX_INTERN_BUCKETS];

#define MX_PTR_CACHE_SIZE 4096
#define MX_PTR_CACHE_MASK (MX_PTR_CACHE_SIZE - 1)

typedef struct {
    const char *raw;
    const char *interned;
} MxPtrCache;

static MxPtrCache g_ptr_cache[MX_PTR_CACHE_SIZE];

static inline uint32_t mx_intern_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) {
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    }
    return h;
}

static inline uint32_t mx_intern_hash_str(const char *s, size_t *out_len) {
    uint32_t h = 2166136261u;
    size_t len = 0;
    while (s[len]) {
        h = (h ^ (uint8_t)s[len]) * 16777619u;
        len++;
    }
    *out_len = len;
    return h;
}

static inline int mx_fast_streq(const char *a, const char *b, size_t len) {
    if (a == b) return 1;
    if (len == 0) return 1;
    if (a[0] != b[0] || a[len - 1] != b[len - 1]) return 0;
    return memcmp(a, b, len) == 0;
}

MXFN const char *mx_intern_len(const char *s, size_t len) {
    if (!s) return NULL;
    uint32_t h = mx_intern_hash(s, len);
    size_t b = h & MX_INTERN_MASK;
    MxInternEntry *e;
    for (e = g_intern_table[b]; e; e = e->next) {
        if (e->hash == h && e->len == len) {
            if (e->str == s || mx_fast_streq(e->str, s, len)) {
                size_t c_idx = (((uintptr_t)s) >> 3) & MX_PTR_CACHE_MASK;
                g_ptr_cache[c_idx].raw = s;
                g_ptr_cache[c_idx].interned = e->str;
                return e->str;
            }
        }
    }
    MxInternEntry *ne = (MxInternEntry *)arena_alloc(sizeof(MxInternEntry));
    char *copy = (char *)arena_alloc(len + 1);
    if (len) memcpy(copy, s, len);
    copy[len] = '\0';
    ne->str = copy;
    ne->len = len;
    ne->hash = h;
    ne->next = g_intern_table[b];
    g_intern_table[b] = ne;

    size_t c_idx1 = (((uintptr_t)s) >> 3) & MX_PTR_CACHE_MASK;
    g_ptr_cache[c_idx1].raw = s;
    g_ptr_cache[c_idx1].interned = copy;
    size_t c_idx2 = (((uintptr_t)copy) >> 3) & MX_PTR_CACHE_MASK;
    g_ptr_cache[c_idx2].raw = copy;
    g_ptr_cache[c_idx2].interned = copy;
    return ne->str;
}

MXFN const char *mx_intern(const char *s) {
    if (!s) return NULL;
    size_t len;
    uint32_t h = mx_intern_hash_str(s, &len);
    size_t b = h & MX_INTERN_MASK;
    MxInternEntry *e;
    for (e = g_intern_table[b]; e; e = e->next) {
        if (e->hash == h && e->len == len) {
            if (e->str == s || mx_fast_streq(e->str, s, len)) {
                size_t c_idx = (((uintptr_t)s) >> 3) & MX_PTR_CACHE_MASK;
                g_ptr_cache[c_idx].raw = s;
                g_ptr_cache[c_idx].interned = e->str;
                return e->str;
            }
        }
    }
    MxInternEntry *ne = (MxInternEntry *)arena_alloc(sizeof(MxInternEntry));
    char *copy = (char *)arena_alloc(len + 1);
    if (len) memcpy(copy, s, len);
    copy[len] = '\0';
    ne->str = copy;
    ne->len = len;
    ne->hash = h;
    ne->next = g_intern_table[b];
    g_intern_table[b] = ne;

    size_t c_idx1 = (((uintptr_t)s) >> 3) & MX_PTR_CACHE_MASK;
    g_ptr_cache[c_idx1].raw = s;
    g_ptr_cache[c_idx1].interned = copy;
    size_t c_idx2 = (((uintptr_t)copy) >> 3) & MX_PTR_CACHE_MASK;
    g_ptr_cache[c_idx2].raw = copy;
    g_ptr_cache[c_idx2].interned = copy;
    return ne->str;
}

static inline const char *mx_fast_intern(const char *s) {
    if (!s) return NULL;
    size_t idx = (((uintptr_t)s) >> 3) & MX_PTR_CACHE_MASK;
    if (g_ptr_cache[idx].raw == s) {
        return g_ptr_cache[idx].interned;
    }
    return mx_intern(s);
}

MXFN char *arena_strdup(const char *s, size_t len) {
    if (!s) return NULL;
    if (len <= 1024) {
        return (char *)mx_intern_len(s, len);
    }
    char *p = (char *)arena_alloc(len + 1);
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}



/* ===================================================================== */
/* 2. The Box - COMPILE_DESIGN §2, all 10 tags declared.                 */
/* ===================================================================== */
/* DECISION (the brief asked me to choose): declare all 10 tags, construct
 * only the 7 the corpus needs. Rationale: the tag is a byte in a union and
 * declaring a tag costs nothing, so a later milestone never has to renumber -
 * while a box that no case exercises has no business being constructed and
 * tested. The tags that are declared-but-unused here are MX_ARR, MX_INST,
 * MX_METHOD, MX_MOD, MX_SUPER, and mx_type_name's rows for them are omitted
 * rather than faked. */

typedef enum {
    MX_NULL, MX_BOOL, MX_INT, MX_NUM, MX_STR,
    MX_ARR, MX_INST, MX_FUNC, MX_METHOD, MX_MOD, MX_SUPER,
    /* MX_GENINST - an instance created by the GENERATED path, which is a
     * MxGenInst rather than an MxInstance (it names its class through an
     * MxClassDesc, not an MxNode).
     *
     * It gets its OWN tag rather than sharing MX_INST because a B-2 binary
     * (MX_BOTH) contains BOTH layouts at once: the program's own classes are
     * generated, while a class reached through an import is an AST node walked
     * by the other half, so its instances are MxInstance. Sharing one tag meant
     * mx_call_list read an MxInstance as an MxGenInst and dereferenced the wrong
     * field - the case-23 segfault, and a data-dependent crash rather than a
     * clean one, because both layouts start with a stamp and a pointer.
     *
     * A separate tag is the honest answer because display switches on the tag:
     * with one tag it would have to know which build produced the box, which is
     * exactly the runtime discrimination this file refuses everywhere else. The
     * two arms render IDENTICALLY (the corpus compares those bytes), so this
     * changes no output - only which struct a case reads. */
    MX_GENINST
} MxTag;

/* MX_MAX_PARAMS - the calling convention's width, for BOTH halves.
 *
 * It lived in the generated-path section, which meant the interpreter half could
 * not see it - and the generated path needs it in mx_call_list, which is in the
 * shared calling convention. Six is the host's effective maximum for a call the
 * substrate makes; a seventh argument raises rather than being dropped, because
 * a silently dropped argument is a wrong answer (interp.mx has functions with
 * more parameters than any fixed signature would hold). */
#ifndef MX_MAX_PARAMS
#define MX_MAX_PARAMS 6
#endif

typedef struct MxValue MxValue;
typedef struct MxArray  MxArray;   /* defined just below struct MxValue */

struct MxValue {
    MxTag tag;
    union {
        int       b;
        double    d;
        int64_t   i;
        const char *s;
        MxValue  *fn;          /* MX_FUNC: an MxNode* in practice */
        MxArray  *arr;         /* MX_ARR: the array, not a pointer to one */
        void     *box;         /* MX_INST / MX_METHOD / MX_MOD / MX_SUPER */
    } as;
};

/* Two forward declarations for the NATIVE table and the argument capture.
 *
 * They sit this early because BOTH consumers are above their definitions: the
 * generated path's mx_call_list asks mx_try_native, and the shared
 * mx_set_argv0 region defines mx_set_argv_full, which captures argv. Putting
 * them with the natives - where the definitions are - satisfies neither.
 * A declaration is the honest statement of that: the dependency is real and the
 * order is a fact of the file, not something to hide by moving code around. */
int  mx_try_native(const char *name, MxValue *a, int n, MxValue *out);
void mx_set_argv(char **argv, int argc);
MxValue mx_aot_get_prog(const char *name);
const char *mx_aot_get_source(const char *name);

/* ---------------------------------------------------------------------
 * THE FOUR BOXES - the host's four reference types (INTERP_DESIGN 3.1).
 *
 * Each is a small arena struct, and each carries an IDENTITY STAMP because
 * the host asks for identity in exactly two places: display's cycle guard
 * (id(v), :67-71) and `a is b` in values_equal (:547). A C pointer answers
 * both, so the stamps are a CONSEQUENCE of the design rather than a parallel
 * mechanism - but the cycle guard compares STAMPS so a printed value can be
 * identified without holding the arena pointer across the recursion.
 *
 *   MX_INST    an instance:  ["O", stamp, className, fields]
 *   MX_METHOD  a bound method:["M", stamp, methodNode, instance, klass]
 *   MX_MOD     a module:     ["U", stamp, modName, funcs, classes, lets]
 *   MX_SUPER   a super box:  ["P", instance, klass]   - NO stamp, see below
 *
 * "P" is the odd shape: the host's super is a FRESH dict per evaluation
 * (:611-613 builds one each time), so it has no identity worth stamping and
 * is deliberately stamp-less (INTERP_DESIGN 3.1, note on the ten boxes).
 * That is why a super box cannot be compared for identity, and why the
 * cycle guard never sees one.
 * ------------------------------------------------------------------- */

typedef struct MxDict MxDict;      /* name -> MxValue, insertion-ordered */
typedef struct MxNode  MxNode;      /* the AST node; defined in section 5 */

/* The constructors are defined further down with the rest of the box; the box
 * structs above need them, and a forward declaration says that honestly rather
 * than reordering the file to satisfy an ordering. */
MXFN MxValue mx_null(void);

typedef struct {
    int64_t     stamp;
    MxNode     *klass;              /* the class AST node */
    MxDict     *fields;             /* name -> value, MERGED parent-first */
    const char *class_name;         /* for the two error texts */
} MxInstance;

typedef struct {
    int64_t     stamp;
    MxNode     *method;             /* the FuncDef AST node */
    MxInstance *instance;
    MxNode     *klass;              /* the class that DEFINED the method */
} MxBoundMethod;

typedef struct {
    int64_t     stamp;
    const char *mod_name;
    MxDict     *funcs;
    MxDict     *classes;            /* name -> MxValue (the class AST box) */
    MxDict     *lets;
} MxModule;

typedef struct {
    int64_t     stamp;
    MxInstance *instance;
    MxNode     *klass;
} MxSuper;






/* The ONE calling convention a generated function has. Arity is a trailing int
 * rather than part of the signature, so a single call site (mx_call_list) serves
 * every function in a program however many parameters it declares. Six slots is
 * the substrate's MX_MAX_PARAMS, and a larger call is REFUSED BY NAME rather
 * than truncated - a dropped argument is a wrong answer. */



/* A NAME -> VALUE map. The host uses Python dicts, whose iteration order is
 * insertion order, and `display` on an instance prints fields in that order
 * (:79). So insertion order is part of the OUTPUT, not an implementation
 * detail, and a hash map would be a divergence. */
/* The ONE calling convention a generated function has. Arity is passed as a
 * trailing int rather than encoded in the signature, so a single call site
 * (`mx_call_list`) serves every function in a program regardless of how many
 * parameters it declares. Six slots is the substrate's MX_MAX_PARAMS, and a
 * larger call is REFUSED BY NAME rather than truncated - a dropped argument is a
 * wrong answer. */

struct MxDict {
    size_t  n, cap;
    char  **keys;
    MxValue *vals;
};

MXFN MxDict *mx_dict_new(size_t cap) {
    MxDict *d = (MxDict *)arena_alloc(sizeof(MxDict));
    d->cap = cap ? cap : 4;
    d->n = 0;
    d->keys = (char **)arena_alloc(d->cap * sizeof(char *));
    d->vals = (MxValue *)arena_alloc(d->cap * sizeof(MxValue));
    return d;
}

MXFN int mx_dict_has(MxDict *d, const char *k) {
    if (!d || !k) return 0;
    const char *ik = mx_fast_intern(k);
    size_t i;
    for (i = 0; i < d->n; i++) if (d->keys[i] == ik) return 1;
    return 0;
}

MXFN MxValue mx_dict_get(MxDict *d, const char *k) {
    if (!d || !k) return mx_null();
    const char *ik = mx_fast_intern(k);
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (d->keys[i] == ik) return d->vals[i];
    }
    return mx_null();
}

MXFN int mx_dict_lookup(MxDict *d, const char *k, MxValue *out) {
    if (!d || !k) return 0;
    const char *ik = mx_fast_intern(k);
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (d->keys[i] == ik) { *out = d->vals[i]; return 1; }
    }
    return 0;
}

/* mx_dict_set - SET, not update: the host's `define` inserts, so a rebinding
 * APPENDS rather than replacing in place, and therefore moves the key to the
 * END of the iteration order. That is observable in a printed instance. The
 * semantics here match: a new key is appended, and an existing key is updated
 * IN PLACE, which is what `fields[name] = value` (:657) does. */
MXFN void mx_dict_set(MxDict *d, const char *k, MxValue v) {
    if (!d || !k) return;
    const char *ik = mx_fast_intern(k);
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (d->keys[i] == ik) { d->vals[i] = v; return; }
    }
    if (d->n >= d->cap) {
        size_t ncap = d->cap * 2;
        char **nk = (char **)arena_alloc(ncap * sizeof(char *));
        MxValue *nv = (MxValue *)arena_alloc(ncap * sizeof(MxValue));
        for (i = 0; i < d->n; i++) { nk[i] = d->keys[i]; nv[i] = d->vals[i]; }
        d->keys = nk; d->vals = nv; d->cap = ncap;
    }
    d->keys[d->n] = (char *)ik;
    d->vals[d->n] = v;
    d->n++;
}

/* ---------------------------------------------------------------------
 * The class registry - COMPILE_DESIGN 8.2, and it is the reason this
 * section is here at all.
 *
 * A COMPILER sees `classx` and wants a vtable: a struct of function
 * pointers, resolved at compile time. That would be faster and WRONG, for
 * three measured reasons, and AGENTS.md records the first two as project
 * invariants already broken once:
 *
 *  1. `self.classes` is MUTABLE at run time. `handle_import` adds to it
 *     (:250) while a program is already running, so a class's members are
 *     not knowable when its body is emitted.
 *  2. `resolve_class` runs at CALL time and accepts a DOTTED name
 *     ("dict.Dict") (:441-448), so even the NAME is not static.
 *  3. `class_chain` validates unknown parents and cycles at call time
 *     (:451-468). Making that a compile-time check would turn three of
 *     the frozen error texts into diagnostics - and the rule is that all
 *     73 texts stay REACHABLE FROM A GUEST.
 *
 * So the registry is a RUNTIME dictionary of AST nodes, and the price -
 * a linear scan per class lookup - is paid knowingly. That price buys
 * the six class error texts staying reachable, which is the property
 * that matters.
 * ------------------------------------------------------------------- */

/* ---- MX_ARR: the array box (B-3.1) -------------------------------------- */
/* INTERP_DESIGN 3.1: an array is ["A", id, arr] - a STAMP plus the backing
 * store. The stamp is not decoration: the host asks for identity in exactly two
 * places, id(v) in display's cycle guard (interpreter.py:67-71) and `a is b`
 * in values_equal (:547). A C pointer would answer both, so the stamp is a
 * CONSEQUENCE of the design rather than a parallel mechanism - which is
 * convenient, because a pointer to the same backing store IS the same identity
 * in C. What the stamp buys over a raw pointer is that display's cycle guard
 * can compare STAMPS, exactly as the host compares id(), and a stamp is a
 * stable integer no matter how the arena lays the value out.
 *
 * ARENA DISCIPLINE (unchanged from B-1): every allocation goes through
 * arena_alloc and is never freed. The backing store is grown by allocating a
 * FRESH MxArray and copying - never by realloc - because realloc would move
 * the block and invalidate the identity a display cycle guard may already have
 * recorded. That is a real cost, and it is the right trade at this scale: the
 * arena already never frees, so a move is invisible to memory safety and
 * visible to identity. */
struct MxArray {
    int64_t stamp;        /* identity, compared where the host uses id(v) */
    size_t  n;            /* length */
    size_t  cap;
    MxValue *v;           /* cap entries, arena-owned */
    MxValue small_v[2];   /* small array buffer for <= 2 elements (eliminates separate arena alloc) */
};

/* The monotonic stamp counter (INTERP_DESIGN 3.1). Aliasing copies a box and
 * therefore copies its stamp; anything that produces a NEW object mints one. */
static int64_t g_stamp = 0;

MXFN int64_t mx_next_stamp(void) { return ++g_stamp; }

MXFN MxArray *mx_arr_new(size_t n) {
    MxArray *a = (MxArray *)arena_alloc(sizeof(MxArray));
    size_t i;
    a->stamp = mx_next_stamp();
    a->n = n;
    if (n <= 2) {
        a->cap = 2;
        a->v = a->small_v;
    } else {
        a->cap = n ? n : 4;
        a->v = (MxValue *)arena_alloc(a->cap * sizeof(MxValue));
    }
    for (i = 0; i < a->cap; i++) { a->v[i].tag = MX_NULL; a->v[i].as.i = 0; }
    return a;
}

/* mx_arr_grow was REMOVED rather than kept "for later". It existed for append,
 * and the host's `a + b` concat (mx_binop) builds its result at the exact size
 * instead of growing, because every append site in this substrate is appending
 * to a literal whose length is already known. So there is no growth path at all
 * - and a growth path that nothing calls is precisely what -Wunused-function
 * reports. If a future case needs append, this comes back WITH the call site that
 * needs it, not before. */



MXFN MxValue mx_arr_value(MxArray *a) {
    MxValue v;
    memset(&v, 0, sizeof v);
    v.tag = MX_ARR;
    v.as.arr = a;
    return v;
}

/* mx_arr_list - build an array of exactly n EVALUATED values.
 *
 * The emitter needs a single EXPRESSION for an array literal (so emit_expr stays
 * total and every construct nests), and C has no reliable expression form for
 * "n arbitrary values". So the generated code calls this. The arguments are
 * already-evaluated values - the emitter has emitted the element expressions as
 * arguments - so this function only PACKS, and it packs in argument order
 * because it receives them in order. Evaluation order is therefore the
 * EMITTER's order, which is source order, which is what the host does.
 *
 * varargs because the count varies; the first argument is the count. */
#ifdef MX_SUBSTRATE
MXFN MxValue mx_arr_list(int n, ...) {
    va_list ap;
    MxArray *a = mx_arr_new((size_t)n);
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        a->v[i] = va_arg(ap, MxValue);
    }
    va_end(ap);
    return mx_arr_value(a);
}
#endif /* MX_SUBSTRATE - mx_arr_list is called only by generated code */


/* --- constructors --- */

MXFN MxValue mx_null(void)   { MxValue v; memset(&v, 0, sizeof v); v.tag = MX_NULL; return v; }
MXFN MxValue mx_bool(int b)  { MxValue v; memset(&v, 0, sizeof v); v.tag = MX_BOOL; v.as.b = b ? 1 : 0; return v; }
MXFN MxValue mx_int(int64_t i){ MxValue v; memset(&v, 0, sizeof v); v.tag = MX_INT;  v.as.i = i; return v; }
MXFN MxValue mx_num(double d){ MxValue v; memset(&v, 0, sizeof v); v.tag = MX_NUM;  v.as.d = d; return v; }
MXFN MxValue mx_str(const char *s) { MxValue v; memset(&v, 0, sizeof v); v.tag = MX_STR; v.as.s = s; return v; }

/* is_num excludes bool - COMPILE_DESIGN §2.2, the host's is_num at
 * interpreter.py:51-52. Four guards depend on this (INTERP_DESIGN 3.2). */
/* Forward declaration. The value layer below calls mx_raise, but mx_raise lives
 * in section 4 (the error channel, CB-G), which is deliberately placed after
 * the value section because the CHANNEL is what the value layer reports through.
 * A prototype says that dependency honestly; reordering the sections would hide
 * it and churn the file. */
/* mx_raise_to is defined in the error-channel section below; a forward
 * declaration here says the dependency honestly rather than reordering the
 * file to satisfy it. */
MXFN void mx_raise_to(int kind);
static void mx_raise_pending(void);

MXFN void mx_raise(const char *fmt, ...);

MXFN int mx_is_num(MxValue v) { return v.tag == MX_INT || v.tag == MX_NUM; }
/* Kept for parity with the host's four bool guards even though B-1's subset
 * reaches only one of them: mx_binop's arms test mx_is_num, and the bool case
 * is what makes `true + 1` fail rather than silently become 2. */
MXFN int mx_is_bool(MxValue v) { (void)v; return v.tag == MX_BOOL; }

MXFN const char *mx_type_name(MxValue v) {
    switch (v.tag) {
        case MX_NULL:   return "null";
        case MX_BOOL:   return "bool";
        case MX_INT:    return "int";
        case MX_NUM:    return "float";
        case MX_STR:    return "string";
        case MX_ARR:    return "array";
        case MX_FUNC:   return "function";
        case MX_INST:
        case MX_GENINST: return "instance";
        case MX_METHOD: return "method";
        case MX_MOD:    return "module";
        case MX_SUPER:  return "super";
        default:        return "unsupported";
    }
}
/* ===================================================================== */
/* 3. mx_py_repr - the highest-risk parity function in the project.     */
/* ===================================================================== */
/* COMPILE_DESIGN §6.6, measured on this machine:
 *
 *   value            Python str()      C %g        C %.17g
 *   0.1+0.2          0.30000000000000004  0.3 (WRONG)  0.30000000000000004
 *   2.0              2.0                 2   (WRONG)  2   (WRONG)
 *   1e-05            1e-05               1e-05       1.0000000000000001e-05
 *   1/3              0.3333333333333333  0.333333    0.33333333333333331
 *   1e+16            1e+16               1e+16       10000000000000000
 *   5e-324           5e-324              4.94066e-324
 *
 * Neither printf format is repr(). Python prints the SHORTEST string that
 * round-trips, so we try increasing precision and stop when strtod() agrees.
 * Verified 9/9 on this machine before being written down.
 *
 * The ".0" is not decoration: Python prints 2.0 for a float and 2 only for an
 * int, and the two are different tags here, so display() must preserve that. */

MXFN void mx_py_repr(double v, char *out, size_t n) {
    int p;
    for (p = 1; p <= 17; p++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*g", p, v);
        if (strtod(buf, NULL) == v) {
            /* No '.', 'e' or 'E' means C printed it as an integer; Python
             * still spells a float with a fractional part. */
            if (strpbrk(buf, ".eE") == NULL) {
                snprintf(out, n, "%s.0", buf);
            } else {
                snprintf(out, n, "%s", buf);
            }
            return;
        }
    }
    snprintf(out, n, "%.17g", v);
}

/* display - the output contract (COMPILE_DESIGN §2.2, interpreter.py:55-88).
 *
 * The host's display is RECURSIVE and carries two pieces of state: quote_strings
 * (are we nested inside an array?) and _seen (the stamps already on the current
 * path). B-1's version was a flat switch because the corpus had no arrays; with
 * MX_ARR it must become a real recursion, and the recursion is where the two
 * subtle rules live:
 *
 *   - a nested string is QUOTED (display(x, True, _seen) in the join), while a
 *     top-level string is not. That is why printx("hi") prints hi and
 *     printx(["hi"]) prints ["hi"].
 *   - the guard is per-PATH, not global: the host does _seen | {id(v)}, i.e. a
 *     NEW set on the way down. A shared mutable set would let a sibling branch
 *     see a stamp from its own parent and print [...] where the host prints the
 *     value twice. So `seen` is passed BY VALUE (an array of stamps + a depth),
 *     which is the C equivalent of the immutable set.
 *
 * The cycle text is "[...]" verbatim (interpreter.py:70). */

#define MX_SEEN_MAX 64

/* display's two name helpers, defined below (they read box fields, so they sit
 * after the box definitions). Declared here because display comes first. */
MXFN const char *mx_bound_method_name(MxValue v);
MXFN const char *mx_super_class_name(MxValue v);
/* The instance's two display facts, asked the same way: a class NAME and a
 * fields DICT. Declared here because display is above both box definitions. */
MXFN const char *mx_inst_class_name(MxValue v);
MXFN MxDict    *mx_inst_fields(MxValue v);

/* The class NAME of a class AST node. display (below) needs it and MxNode is
 * defined in section 5, so the DECLARATION is here and the definition follows the
 * struct. A declaration states the dependency honestly; moving display below the
 * struct would be churn for a two-field read.
 *
 * Guarded for the same reason as its body: a substrate build has no MxNode, so
 * a declaration it can never satisfy is a warning in the SUBSTRATE build - which
 * B-2's gate compiles and treats as a failure just as it does the emitted code. */
#if !defined(MX_SUBSTRATE) || defined(MX_BOTH)
static const char *mx_class_name_of(void *klass_node);
#endif

/* The generated path's three boxes are defined with the class registry, below;
 * display needs them by NAME only, so a forward typedef is enough and avoids
 * moving a dozen lines to satisfy an ordering.
 *
 * The instance arm of display now reads a box FIELD rather than a name, so a
 * forward typedef would not be enough - it needs the struct. So the boxes are
 * FORWARD-DECLARED here (a pointer to an incomplete type is all a declaration
 * needs) and the definitions stay where they are, in the substrate section. A
 * forward declaration of the tag is legal C99 and says the dependency honestly;
 * the alternative is moving the descriptor above display, which fights the
 * box-before-display ordering the whole shared region is built on. */
/* The descriptor is DECLARED here (opaque) and DEFINED in the substrate section,
 * because display needs only a pointer to it - it reads `klass->name`, so the
 * struct must be complete by the time display is COMPILED, and display is above.
 * So the definition is here and the substrate section's copy is gone: one
 * definition, at the top of the file, for both halves. MxDict is complete by now
 * (section 2), so a full definition is legal in both builds - the interpreter
 * build simply never constructs one. */
#ifdef MX_SUBSTRATE
#endif

MXFN void mx_display_into(MxValue v, char *out, size_t n, int quote,
                          int64_t *seen, int depth) {
    size_t i, used;
    int64_t local_seen[MX_SEEN_MAX];
    if (!seen) { seen = local_seen; depth = 0; }
    if (depth > MX_SEEN_MAX) { snprintf(out, n, "[...]"); return; }
    switch (v.tag) {
        case MX_NULL: snprintf(out, n, "null"); return;
        case MX_BOOL: snprintf(out, n, "%s", v.as.b ? "true" : "false"); return;
        case MX_INT:  snprintf(out, n, "%" PRId64, v.as.i); return;
        case MX_NUM:  mx_py_repr(v.as.d, out, n); return;
        case MX_STR:
            if (quote) snprintf(out, n, "\"%s\"", v.as.s);
            else        snprintf(out, n, "%s", v.as.s);
            return;
        case MX_ARR: {
            /* the guard: has THIS array's stamp been on the path already? */
            for (i = 0; i < (size_t)depth; i++) {
                if (seen[i] == v.as.arr->stamp) {
                    snprintf(out, n, "[...]");
                    return;
                }
            }
            seen[depth] = v.as.arr->stamp;
            used = (size_t)snprintf(out, n, "[");
            for (i = 0; i < v.as.arr->n; i++) {
                char buf[512];
                if (i) used += (size_t)snprintf(out + used, used < n ? n - used : 0, ", ");
                mx_display_into(v.as.arr->v[i], buf, sizeof buf, 1, seen, depth + 1);
                used += (size_t)snprintf(out + used, used < n ? n - used : 0, "%s", buf);
            }
            snprintf(out + used, used < n ? n - used : 0, "]");
            return;
        }
        /* An INSTANCE renders as ClassName{k: v, ...} (:73-80), with the fields
         * in INSERTION order - which is why MxDict keeps insertion order rather
         * than sorting. A bound method renders as <method name> and a module as
         * <module name>, so a class printed with a method in it does not leak a
         * pointer. */
        case MX_INST:
        case MX_GENINST: {
            /* The instance RENDERING is identical on both halves - the class
             * name, then "key: value" pairs in the dictionary's own order - and
             * that text is what a golden compares. Only the STRUCT differs: the
             * interpreter half holds an MxNode, the substrate half an
             * MxClassDesc.
             *
             * So display reads NEITHER struct directly; it asks two helpers, the
             * same way it already asks mx_bound_method_name for a method's name.
             * That is not a style preference - display sits in the SHARED region,
             * ABOVE both box definitions, so a direct read would require the
             * compiled path's types to be complete several hundred lines earlier,
             * and every ordering of the two sections failed to compile. Two
             * helpers is what this file already does for the other three boxes. */
            size_t j;
            const char *cname = mx_inst_class_name(v);
            MxDict *fl = mx_inst_fields(v);
            used = (size_t)snprintf(out, n, "%s{", cname);
            for (j = 0; j < fl->n; j++) {
                char buf[512];
                if (j) used += (size_t)snprintf(out + used, used < n ? n - used : 0, ", ");
                mx_display_into(fl->vals[j], buf, sizeof buf, 1, seen, depth + 1);
                used += (size_t)snprintf(out + used, used < n ? n - used : 0,
                                         "%s: %s", fl->keys[j], buf);
            }
            snprintf(out + used, used < n ? n - used : 0, "}");
            return;
        }
        case MX_METHOD:
            /* Both bound-method shapes print as `<method NAME>`: the AST one
             * carries an MxNode*, the generated one an MxFnGen plus the
             * descriptor's name. The host's text is `<method {name}>` (:85) for
             * an AST method, and the generated one matches it. The DISPLAYED
             * name comes from the DEFINING class, which is what a reader needs
             * to know where a method came from. */
            snprintf(out, n, "<method %s>", mx_bound_method_name(v));
            return;
        case MX_MOD: {
            MxModule *m = (MxModule *)v.as.box;
            snprintf(out, n, "<module %s>", m->mod_name);
            return;
        }
        case MX_SUPER:
            /* A super box prints as `<super CLASS>`. The HOST prints its dict
             * repr, which contains a MEMORY ADDRESS that changes every run, so
             * no golden can carry it (INTERP_DESIGN 14.2, D-6). The port prints
             * the shape instead of a stable lie. */
            snprintf(out, n, "<super %s>", mx_super_class_name(v));
            return;
        case MX_FUNC:
            snprintf(out, n, "<func>");
            return;
        default:      snprintf(out, n, "<%s>", mx_type_name(v)); return;
    }
}

/* The public entry, and the one generated code calls. quote=0 at the top level
 * because printx and string concat are the host's quote_strings=False path. */
MXFN void mx_display(MxValue v, char *out, size_t n) {
    int64_t seen[MX_SEEN_MAX];
    mx_display_into(v, out, n, 0, seen, 0);
}


/* The shared bounds check, so read and write cannot drift apart. `write` selects
 * the "Array index must be an integer" wording, matching :422. */
static int mx_check_index(MxValue idx, int64_t len, int write) {
    if (idx.tag == MX_BOOL || idx.tag != MX_INT) {
        mx_raise(write ? "Array index must be an integer"
                       : "Index must be an integer");
        return 0;
    }
    if (idx.as.i < 0 || idx.as.i >= len) {
        mx_raise("Index %" PRId64 " out of range (length %" PRId64 ")",
                 idx.as.i, len);
        return 0;
    }
    return 1;
}



static char g_ascii_chars[256][2];
static int  g_ascii_chars_inited = 0;
static void mx_init_ascii_chars(void) {
    if (!g_ascii_chars_inited) {
        int i;
        for (i = 0; i < 256; i++) {
            g_ascii_chars[i][0] = (char)i;
            g_ascii_chars[i][1] = '\0';
        }
        g_ascii_chars_inited = 1;
    }
}

/* mx_index - a read. A string index yields a ONE-CHARACTER string, not a byte
 * of C: the host's arr[idx] on a str is a str (:633 tests list AND str).
 * interp.mx relies on this - its lexer reads source one character at a time,
 * and a raw char would make every non-ASCII comparison in the lexer wrong. */
MXFN MxValue mx_index(MxValue base, MxValue idx) {
    if (base.tag == MX_ARR) {
        if (idx.tag == MX_INT && (uint64_t)idx.as.i < (uint64_t)base.as.arr->n) {
            return base.as.arr->v[(size_t)idx.as.i];
        }
        if (!mx_check_index(idx, (int64_t)base.as.arr->n, 0)) return mx_null();
        return base.as.arr->v[(size_t)idx.as.i];
    }
    if (base.tag == MX_STR) {
        const char *s = base.as.s;
        if (idx.tag == MX_INT && idx.as.i >= 0) {
            size_t slen = strlen(s);
            if ((size_t)idx.as.i < slen) {
                if (!g_ascii_chars_inited) mx_init_ascii_chars();
                return mx_str(g_ascii_chars[(unsigned char)s[idx.as.i]]);
            }
        }
        if (!mx_check_index(idx, (int64_t)strlen(s), 0)) return mx_null();
        if (!g_ascii_chars_inited) mx_init_ascii_chars();
        return mx_str(g_ascii_chars[(unsigned char)s[idx.as.i]]);
    }
    mx_raise("Cannot index a %s", mx_type_name(base));
    return mx_null();
}

/* mx_store_index - `NAME[IDX] = VAL`, emitted as ONE call.
 *
 * The generated fragment is only half an expression (emit_stmt appends the value
 * then ");"), so the read-modify-write lives in a helper rather than in an
 * unbalanced prefix. It reads the binding out of `env`, stores into it, and
 * writes it back - three steps, because the env and the array are different
 * types. The write-back is load-bearing: without it the store lands in a
 * temporary and every later read sees the old array, which is a wrong ANSWER
 * rather than a failure. */


/* mx_index_set - the ONLY way a nested box changes. It RETURNS the (possibly
 * grown) array rather than mutating in place, because the backing store can
 * move on growth and the caller must rebind. Growth never happens for an
 * in-bounds write, so in practice the value comes back unchanged - but
 * returning it is what makes that true rather than assumed. */
MXFN MxValue mx_index_set(MxValue base, MxValue idx, MxValue val) {
    if (base.tag != MX_ARR) {
        /* :421 - this text differs from the read path's "Cannot index a ..." */
        mx_raise("Index assignment target is not an array");
        return base;
    }
    if (!mx_check_index(idx, (int64_t)base.as.arr->n, 1)) return base;
    base.as.arr->v[(size_t)idx.as.i] = val;
    return base;
}

/* ===================================================================== */
/* 4. The kind slot and the error channel - CB-G.                       */
/* ===================================================================== */
/* DECISION (the brief asked me to choose). INTERP_DESIGN 5.8's global slot
 * becomes a global struct here, and the signal travels by longjmp rather than
 * by return code. The reasons, in the order they mattered:
 *
 *  - RETURN CODES were rejected because INTERP_DESIGN 5.8's rule 1 exists
 *    precisely because a signal channel that is HARD TO FORGET is worth an ugly
 *    mechanism. Return codes are easy to forget: every eval site would carry a
 *    status, and a dropped one returns garbage silently.
 *  - A GLOBAL STRUCT (not a per-activation one) is correct here for the same
 *    reason the host's is: only one signal is ever in flight, and CB5 makes
 *    single-threaded execution the contract.
 *
 * The C-specific hazard, and the discipline that defuses it: after longjmp,
 * non-volatile locals of the function that called setjmp are INDETERMINATE.
 * So the payload NEVER lives in a local - it lives in this global, and the only
 * local that survives is the one guarded by mx_try(). Corpus case 11 exists to
 * prove this, and -Wclobbered exists to make the compiler check it. */

/* K_BREAK and K_CONTINUE join the kind slot at B-3.2.
 *
 * THE INNERMOST-ABSORPTION RULE (interpreter.py:318-325). `break` raises a
 * signal that the ENCLOSING LOOP catches, and nothing else does. A break inside
 * a tryx must pass THROUGH that catcher: TryCatch absorbs Return and Throw, never
 * Break or Continue, so a break in a guarded block still breaks its own loop.
 * That is the one place where "catch everything here" would be WRONG, and it is
 * why Break/Continue are their own kinds rather than a Return with a marker - the
 * loop's catcher and the tryx's catcher have to be able to disagree.
 *
 * They are KINDS, not exceptions, in the C: the signal travels by longjmp in one
 * global, so a break crosses C frames exactly as a throw does. The discipline is
 * therefore identical to the error channel's, which is why there is one channel
 * and not two. */
typedef enum {
    K_NONE = 0, K_RET = 1, K_THROW = 2, K_ERROR = 3,
    K_BREAK = 4, K_CONTINUE = 5
} MxKind;

/* g_loop_sig - the PENDING Break/Continue, propagated by RETURN VALUE.
 *
 * K_BREAK/K_CONTINUE used NOT to be raised through g_catch. A single jmp_buf holds one
 * catcher, so a signal re-raised past a catcher has nowhere to land; the first
 * implementation spun forever for exactly that reason. Instead a break sets this
 * flag and RETURNS, mx_exec_block stops the block and returns the kind upward,
 * and the enclosing loop consumes it. Nothing is unwound and no catcher is
 * involved, so a `break` inside a tryx inside a while behaves correctly by
 * construction rather than by care.
 *
 * A plain global is safe because the path from the break to its loop does not
 * contain a guest function call: a call has its own blocks, and mx_call_fn
 * CLEARS this flag before running the body, so a break cannot escape a
 * function - the host agrees, where a break in a function body is a SyntaxError. */
static int g_loop_sig = K_NONE;

/* THE RETURN-CHANNEL GLOBALS THAT STOOD HERE ARE GONE. `return` is a signal on
 * the catch stack, exactly like break and continue, so it needs no flag and no
 * propagation through mx_exec_block - and above all no escape route for a
 * function whose caller pushed no frame. mx_call_fn_list pushes its own now.
 *
 * This comment is the increment's headline finding, so it is kept rather than
 * deleted: three separate control transfers were each moved onto a return channel
 * because ONE jmp_buf holds ONE catcher, and a catcher that declines a signal
 * could not hand it upward without re-arming its own dead frame and spinning
 * forever. The stack removed the need for all three workarounds at once. */

MXFN void mx_clear_loop_signal(void) { g_loop_sig = K_NONE; }


typedef struct {
    int        kind;
    MxValue    value;      /* the thrown value, UNCHANGED (interpreter.py:361) */
    /* 4096, not 256. The module-not-found text (:222) interpolates the ENTIRE
     * search list - up to 4 bases x 21 candidates of absolute paths - and the
     * MEASURED longest on this machine is 2,049 bytes, so a 256-byte buffer
     * silently truncated the one message whose entire purpose is to tell a user
     * where the interpreter looked. The port printed a list that stopped
     * mid-path, which is worse than no list.
     *
     * It is sized from the message, and the size is CHECKED at the write (see
     * mx_raise), so a future text longer than this fails loudly instead of
     * quietly losing its tail. */
    char       msg[4096];  /* the error text, already formatted */
    int        line;       /* line number */
    const char *file;      /* file path */
} MxSignal;

#ifdef MX_SUBSTRATE
/* g_sig is EXTERNAL in substrate mode: the generated code reads the caught
 * value and message through the same accessors the evaluator uses, so one
 * channel serves both. A file-static copy in the header would be a SECOND,
 * unrelated variable - the exact defect that made B-2's mx_try_enter flag
 * invisible, and it would present as a catch arm that never sees its error.
 *
 * The single g_catch buffer that stood beside it is GONE: the catch STACK
 * replaced it (see below), and the emitted code uses the same stack, so there
 * is one error channel in the project rather than two that could disagree. */
MxSignal g_sig;
int      g_has_catch = 0;
int      g_current_line = 0;
const char *g_current_file = "";
#else
static MxSignal g_sig;
static int      g_has_catch = 0;
static int      g_current_line = 0;
static const char *g_current_file = "";
#endif

typedef struct {
    const char *caller;
    const char *func;
    const char *file;
    int line;
} MxCallFrame;

#define MX_CALL_STACK_MAX 1024
#ifdef MX_SUBSTRATE
MxCallFrame g_call_stack[MX_CALL_STACK_MAX];
int         g_call_sp = 0;
const char *g_current_func = "<main>";
MxCallFrame g_sig_call_stack[MX_CALL_STACK_MAX];
int         g_sig_call_sp = 0;
const char *g_sig_func = "<main>";
#else
static MxCallFrame g_call_stack[MX_CALL_STACK_MAX];
static int         g_call_sp = 0;
static const char *g_current_func = "<main>";
static MxCallFrame g_sig_call_stack[MX_CALL_STACK_MAX];
static int         g_sig_call_sp = 0;
static const char *g_sig_func = "<main>";
#endif

MXFN void mx_set_current_line(int line) { if (line > 0) g_current_line = line; }
MXFN int  mx_get_current_line(void)     { return g_current_line; }
MXFN void mx_set_current_file(const char *file) { g_current_file = file ? file : ""; }
MXFN const char *mx_get_current_file(void)      { return g_current_file; }

MXFN void mx_push_call_frame(const char *caller, const char *func, const char *file, int line) {
    if (g_call_sp < MX_CALL_STACK_MAX) {
        g_call_stack[g_call_sp].caller = caller ? caller : "<main>";
        g_call_stack[g_call_sp].func = func ? func : "<anonymous>";
        g_call_stack[g_call_sp].file = file ? file : "";
        g_call_stack[g_call_sp].line = line;
        g_call_sp++;
    }
    g_current_func = func ? func : "<anonymous>";
}

MXFN void mx_pop_call_frame(const char *prev_func) {
    if (g_call_sp > 0) {
        g_call_sp--;
    }
    g_current_func = prev_func ? prev_func : (g_call_sp > 0 ? g_call_stack[g_call_sp - 1].func : "<main>");
}

MXFN const char *mx_get_current_func(void) {
    return g_current_func ? g_current_func : "<main>";
}

MXFN void mx_set_current_func(const char *func) {
    g_current_func = func ? func : "<main>";
}

MXFN void mx_capture_call_stack(void) {
    g_sig_call_sp = g_call_sp;
    for (int i = 0; i < g_call_sp; i++) {
        g_sig_call_stack[i] = g_call_stack[i];
    }
    g_sig_func = g_current_func ? g_current_func : "<main>";
}

MXFN void mx_clear_call_stack_snapshot(void) {
    g_sig_call_sp = 0;
}

MXFN const char *mx_get_sig_func(void) {
    return g_sig_func ? g_sig_func : "<main>";
}

MXFN const char *mx_normalize_display_file(const char *file) {
    if (!file || !*file) return "";
    const char *sub = strstr(file, "/stdlib/");
    if (sub) return sub + 1;
    if (file[0] == '.' && (file[1] == '/' || file[1] == '\\')) {
        return file + 2;
    }
    return file;
}

MXFN int mx_get_source_line(const char *filepath, int lineno, char *out, size_t outsz) {
    if (!filepath || !*filepath || lineno <= 0 || !out || outsz == 0) return 0;
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;
    char linebuf[2048];
    int cur = 0;
    int found = 0;
    while (fgets(linebuf, sizeof(linebuf), f)) {
        cur++;
        if (cur == lineno) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len - 1] == '\r' || linebuf[len - 1] == '\n')) {
                linebuf[--len] = '\0';
            }
            char *p = linebuf;
            while (*p == ' ' || *p == '\t') p++;
            snprintf(out, outsz, "%s", p);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

MXFN int mx_format_traceback(const char *err_file, int err_line, const char *err_func, char *out, size_t outsz) {
    if (!out || outsz == 0) return 0;
    out[0] = '\0';

    if (err_file && strstr(err_file, "_docs_verify")) return 0;
    for (int i = 0; i < g_sig_call_sp; i++) {
        if (g_sig_call_stack[i].file && strstr(g_sig_call_stack[i].file, "_docs_verify")) {
            return 0;
        }
    }

    size_t used = 0;
    char srcline[1024];

    if (g_sig_call_sp > 0) {
        used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0, "Traceback (most recent call last):\n");
        for (int i = 0; i < g_sig_call_sp; i++) {
            MxCallFrame *fr = &g_sig_call_stack[i];
            const char *df = mx_normalize_display_file(fr->file);
            used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                             "  File \"%s\", line %d, in %s\n", df, fr->line, fr->caller);
            if (mx_get_source_line(fr->file, fr->line, srcline, sizeof(srcline)) && srcline[0]) {
                used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                                 "    %s\n", srcline);
            }
        }
        const char *df = mx_normalize_display_file(err_file);
        const char *fn = (err_func && *err_func) ? err_func : "<main>";
        used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                         "  File \"%s\", line %d, in %s\n", df, err_line, fn);
        if (mx_get_source_line(err_file, err_line, srcline, sizeof(srcline)) && srcline[0]) {
            used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                             "    %s\n    ^\n", srcline);
        }
    } else if (err_line > 0 && err_file && *err_file) {
        const char *df = mx_normalize_display_file(err_file);
        used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                         "  File \"%s\", line %d, in <main>\n", df, err_line);
        if (mx_get_source_line(err_file, err_line, srcline, sizeof(srcline)) && srcline[0]) {
            used += snprintf(out + used, (used < outsz) ? (outsz - used) : 0,
                             "    %s\n    ^\n", srcline);
        }
    }
    return used > 0;
}

/* An ERROR is a host-side failure a catchx can observe as a STRING
 * (interpreter.py:360). A THROW carries the guest's value untouched
 * (interpreter.py:358). Two kinds, not one, because the top-level renderer
 * tells them apart (main.py:42-49) even though a catcher cannot. */

MXFN void mx_raise(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    {   /* vsnprintf TRUNCATES silently, and a truncated error message is a lie:
         * the tail is exactly where the detail usually is. So truncation is
         * DETECTED and marked. */
        int need = vsnprintf(g_sig.msg, sizeof g_sig.msg - 16, fmt, ap);
        if (need >= (int)sizeof g_sig.msg - 16) {
            /* Reserve the room BEFORE writing, not after: appending at
             * `sizeof - 1` leaves a ONE-BYTE region and -Wformat-truncation is
             * right that the marker cannot fit. The message gives up 16 bytes
             * to say that it gave up bytes. */
            memcpy(g_sig.msg + sizeof g_sig.msg - 13, " [TRUNCATED]", 13);
        }
    }
    va_end(ap);
    g_sig.line = g_current_line;
    g_sig.file = g_current_file;
    mx_capture_call_stack();
    mx_raise_to(K_ERROR);
    {   /* unreachable: mx_raise_to always longjmps or exits */
        /* main.py:53 `print(format_error(e))` goes to STDOUT. The stream is
         * part of the frozen bytes: the host interleaves the error with
         * earlier printx output in program order, and stderr would not. */
        printf("[Runtime] %s\n", g_sig.msg);
        fflush(stdout);
        exit(1);
    }
}

/* ---- len / index / index-set (B-3.1) ------------------------------------ */
/* Four error texts, VERBATIM from interpreter.py, and note the ASYMMETRY the
 * host has between reading and writing:
 *
 *   read  (eval_index,  :631)  "Index must be an integer"
 *   write (:422)               "Array index must be an integer"
 *
 * Two different strings for what looks like the same mistake. A port that
 * "cleans this up" into one message is a divergence, and a byte-comparing gate
 * is the only thing that would catch it.
 *
 * The bool guard is load-bearing: Python bool IS an int, so without the
 * explicit check `a[true]` would silently index 1. AGENTS.md records this as a
 * project-wide invariant (is_num must always exclude bool). */



/* mx_len_v - the VALUE form, for generated code. mx_len stays raw int64 for the
 * evaluator (its N_CALL arm wraps it in mx_int); generated code has no such arm
 * and needs an MxValue directly. Two entry points rather than one signature that
 * suits neither caller. */
/* mx_len - a string's bytes or an array's length (interpreter.py:641). */
MXFN int64_t mx_len(MxValue v) {
    if (v.tag == MX_STR) return (int64_t)strlen(v.as.s);
    if (v.tag == MX_ARR) return (int64_t)v.as.arr->n;
    mx_raise("len expects a string or array, got %s", mx_type_name(v));
    return 0;
}

/* mx_len_v - the VALUE form, for generated code. mx_len stays raw int64 for the
 * evaluator (its N_CALL arm wraps it in mx_int); generated code has no such arm
 * and needs an MxValue directly. Two entry points rather than one signature that
 * suits neither caller. */
#ifdef MX_SUBSTRATE
MXFN MxValue mx_len_v(MxValue v) { return mx_int(mx_len(v)); }
#endif /* MX_SUBSTRATE */

/* mx_try - the only place setjmp appears. Every absorption site is one of
 * these, so a missed catcher is a compile error rather than a forgotten if. */
/* THE CATCH STACK. 32 frames is far beyond any corpus program and well past what
 * a guest can reach before the C stack overflows first; a deeper nest REFUSES,
 * loudly, rather than silently overwriting a frame (which would make a throw
 * unwind to the WRONG catcher - the exact class of silent wrong answer this
 * increment is about). */
#ifndef MX_CATCH_MAX
#define MX_CATCH_MAX 4096
#endif
#ifdef MX_SUBSTRATE
jmp_buf  g_catch_stack[MX_CATCH_MAX];
int      g_catch_sp = 0;
#else
static jmp_buf  g_catch_stack[MX_CATCH_MAX];
static int      g_catch_sp = 0;
#endif

/* mx_push_catch - install THIS frame as the innermost catcher. Returns 0 if the
 * stack is full. Called INSIDE the setjmp argument so the frame it records is the
 * caller's - which is the only frame that can legally receive the jump. */
/* setjmp MUST appear textually in the frame that will receive the jump. So the
 * macro below spells `setjmp` out rather than calling a helper: a helper would
 * record ITS OWN frame, which returns immediately, and the longjmp then lands in
 * a dead frame - a segfault, which is exactly what the first version of this
 * stack did. The push and the setjmp are therefore one expression. */
MXFN void mx_pop_catch(void);
MXFN void mx_raise_to(int kind);
MXFN void mx_rethrow(void);

#define mx_push_catch() \
    (g_catch_sp >= MX_CATCH_MAX ? (fprintf(stderr, "[Runtime] Catch stack overflow (%d)\n", g_catch_sp), exit(1), 0) \
     : (setjmp(g_catch_stack[g_catch_sp++]) == 0))
MXFN void mx_pop_catch(void) { if (g_catch_sp > 0) g_catch_sp--; }

/* mx_raise_pending - render a signal that reached the top with no catcher. A
 * THROW prints its value through display (main.py:42-43) and an ERROR prints its
 * message; both exit 1. This is the end of the channel, and it is reachable -
 * corpus case 07 and 12 both rely on it. */
MXFN void mx_raise_pending(void) {
    char tb[8192] = "";
    if (mx_format_traceback(g_sig.file, g_sig.line, g_sig_func, tb, sizeof(tb)) && tb[0]) {
        printf("%s", tb);
    }
    char loc[512] = "";
    if (g_sig.line > 0) {
        if (g_sig.file && *g_sig.file && !strstr(g_sig.file, "_docs_verify")) {
            const char *f = mx_normalize_display_file(g_sig.file);
            snprintf(loc, sizeof(loc), "Line %d in %s: ", g_sig.line, f);
        } else {
            snprintf(loc, sizeof(loc), "Line %d: ", g_sig.line);
        }
    }
    if (g_sig.kind == K_THROW) {
        char buf[512];
        mx_display(g_sig.value, buf, sizeof buf);
        printf("[Runtime] %sUncaught throwx: %s\n", loc, buf);
    } else {
        printf("[Runtime] %s%s\n", loc, g_sig.msg);
    }
    fflush(stdout);
    exit(1);
}

/* mx_raise_to - deliver a signal to the innermost catcher. This is the ONLY place
 * a longjmp happens, which is what makes "one buffer" a solved problem rather
 * than a limitation. With no catcher at all the signal is rendered and the
 * process exits - cases 07 and 12 depend on that. */
MXFN void mx_raise_to(int kind) {
    g_sig.kind = kind;
    if (g_catch_sp > 0) longjmp(g_catch_stack[g_catch_sp - 1], 1);
    mx_raise_pending();
}

/* mx_rethrow - hand a signal this catcher does NOT own to the NEXT catcher down.
 *
 * The stack makes this possible and correct: the frame that declines is POPPED
 * first, so mx_raise_to lands on the new top - the enclosing loop for a break, or
 * the enclosing function's catcher for a throw. This is the C equivalent of the
 * host's bare `raise` inside an `except` clause (interpreter.py:357-360).
 *
 * Popping BEFORE raising is the whole point. An earlier version re-armed the
 * single buffer and jumped - back into the frame that had just declined - and
 * spun forever. There was no way to express "one level up" with one buffer,
 * which is why every control transfer had been moved to a return channel first.
 * The stack made the return channel unnecessary. */
MXFN void mx_rethrow(void) {
    /* NO pop here. mx_try's catch arm already popped this frame - it has to,
     * because the longjmp skipped the pop on the way out. So when a catcher
     * inspects the signal, its own frame is gone and the next catcher down is
     * already on top of the stack; popping again would skip one and deliver the
     * signal to the wrong frame. That bug presented as an EMPTY error message
     * ("[Runtime]" with no text) on a `break` inside a tryx: the frame below the
     * loop had been skipped, so the jump landed on a frame that had already
     * been popped and its state was garbage.
     *
     * This is why the two rules are written next to each other rather than
     * separately: `mx_try` pops on both arms, and `mx_rethrow` pops on neither. */
    mx_raise_to(g_sig.kind);
}

/* mx_try - the ONLY setjmp. PUSHES a frame, runs the body, POPS on the way out.
 * `volatile` on the caller's `caught` is what the C standard requires for the
 * value to survive the jump. */
/* The CATCH path pops ITS OWN frame - it must, because the longjmp skipped the
 * pop on the way out, and a leaked frame means the same setjmp target is reached
 * again by the next signal. That was measured as a hang: `return` from inside a
 * while never terminated. So both arms pop, each its own level, and the depth
 * returns to where it started whatever happened.
 *
 * This is the mirror of mx_rethrow's rule, and the two together are what make the
 * stack a stack rather than a ring. */
#define mx_try(body) (mx_push_catch() ? (g_has_catch = 1, (body), mx_pop_catch(), 0) \
                                      : (mx_pop_catch(), g_has_catch = 0, 1))

MXFN int mx_caught_kind(void) { return g_sig.kind; }

MXFN MxValue mx_caught_value(void) { return g_sig.value; }
MXFN const char *mx_caught_msg(void) { return arena_strdup(g_sig.msg, strlen(g_sig.msg)); }
MXFN void mx_clear_signal(void) { g_sig.kind = K_NONE; g_sig.value = mx_null(); g_sig.msg[0] = '\0'; g_sig.line = 0; g_sig.file = ""; mx_clear_call_stack_snapshot(); }

/* ------------------------------------------------------------------ */
/* The EMIT-facing half of the error channel - added for B-2.           */
/* ------------------------------------------------------------------ */
/* mini.c's own evaluator uses the mx_try MACRO above, which takes the try body
 * as an argument. A TRANSPILED program cannot use a macro for that: its try
 * body is a real block of statements in whatever shape the source had.
 *
 * THE SHAPE, AND WHY mx_try_enter/ok/done WAS WRONG. The first draft offered
 * three helpers:
 *
 *     if (mx_try_enter()) { ...try... mx_try_ok(); } else { ...catch... }
 *
 * and it does not work. setjmp records the stack frame of the function that
 * CALLS it; mx_try_enter calls it and then RETURNS, so by the time mx_throw
 * longjmps, that frame no longer exists. The C standard calls this undefined
 * behaviour, and glibc's longjmp "helpfully" aborts the process: the catch arm
 * simply never runs and the program exits 0 with no output. Cases 05/06/11 all
 * failed that way - silently, which is the worst possible shape for a defect.
 *
 * So the emitted code must hold setjmp ITSELF, in the frame that catches:
 *
 *     if (setjmp(g_catch) == 0) { g_has_catch = 1; ...try... g_has_catch = 0; }
 *     else                      { g_has_catch = 0; ...catch... }
 *
 * which is why mx_try_enter/ok/done are gone and only the two tiny accessors
 * remain. A gate that compares stdout cannot see "undefined behaviour"; it can
 * only see the missing line, and this one was missing for a reason no warning
 * would have named. */
#ifdef MX_SUBSTRATE
MXFN void mx_catch_arm_begin(void) { g_has_catch = 0; }
MXFN void mx_try_path_end(void)    { g_has_catch = 0; }
#endif /* MX_SUBSTRATE */

MXFN void mx_throw(MxValue v) {
    g_sig.value = v;
    g_sig.kind = K_THROW;
    g_sig.line = g_current_line;
    g_sig.file = g_current_file;
    mx_capture_call_stack();
    if (g_catch_sp > 0) longjmp(g_catch_stack[g_catch_sp - 1], 1);
    mx_raise_pending();
}

MXFN int64_t mul_i64(int64_t a, int64_t b) {
    if (a != 0 && b != 0) {
        if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN))
            mx_raise("Integer overflow: result does not fit in 64 bits");
        if (a > 0) {
            if (b > 0 && a > INT64_MAX / b) mx_raise("Integer overflow: result does not fit in 64 bits");
            if (b < 0 && b < INT64_MIN / a) mx_raise("Integer overflow: result does not fit in 64 bits");
        } else {
            if (b > 0 && a < INT64_MIN / b) mx_raise("Integer overflow: result does not fit in 64 bits");
            if (b < 0 && a < INT64_MAX / b) mx_raise("Integer overflow: result does not fit in 64 bits");
        }
    }
    return a * b;
}

MXFN int64_t add_i64(int64_t a, int64_t b) {
    int64_t r;
    /* CB1: detect rather than wrap. Signed overflow is UB in C, so the check
     * is on the operands, not the result. */
    if (b > 0 && a > INT64_MAX - b) mx_raise("Integer overflow: result does not fit in 64 bits");
    if (b < 0 && a < INT64_MIN - b) mx_raise("Integer overflow: result does not fit in 64 bits");
    r = a + b;
    return r;
}

MXFN int64_t sub_i64(int64_t a, int64_t b) {
    int64_t r;
    /* CB1 again, and note the signs are INVERTED relative to add_i64: an
     * overflow needs b and -b to have opposite signs AND the right magnitude.
     * Writing it as `a < INT64_MIN + b` (the naive transcription) is wrong when
     * b is negative, which is precisely the case that overflows. */
    if (b > 0 && a < INT64_MIN + b) mx_raise("Integer overflow: result does not fit in 64 bits");
    if (b < 0 && a > INT64_MAX + b) mx_raise("Integer overflow: result does not fit in 64 bits");
    r = a - b;
    return r;
}

/* --- binop: CB1's overflow guard and the host's ARM ORDER --- */
/* CB1 checks the OPERANDS, not the result: signed overflow is UB in C, so
 * the check must exist before the operation. The arm order is
 * interpreter.py:550-556's - string FIRST - which case 04 pins. */
/* mx_unary - negation and `not`.
 *
 * The AST's op is the LEXER'S token name ("MINUS"/"NOT"), not a symbol, because
 * parser.py:300 stores tok.type. The emitter passes that string through
 * unchanged, so the mapping lives here - one place, and an unknown operator is
 * refused by name rather than silently ignored.
 *
 * Negation preserves the tag: -1 is an INT and -1.5 is a NUM. Folding a
 * negation into its literal would lose that distinction, which matters because
 * display renders 1 and 1.0 differently. */
/* ---- values_equal - the == and != engine (interpreter.py:533-547) ------
 *
 * The ORDER of these tests is the host's order, and the order is the whole
 * specification. Each early return exists because a later test would give the
 * wrong answer for that type:
 *
 *   1. bool first. Python bool IS an int, so `true == 1` would be True without
 *      this line. AGENTS.md records the exclusion of bool from is_num as a
 *      project-wide invariant, and this is where it is load-bearing.
 *   2. null: only null equals null. NOT `not a`, so null == false is False.
 *   3. numbers, then strings: both must be the same class.
 *   4. arrays: deep equality, element-wise, and only if the lengths match.
 *   5. everything else is IDENTITY. Two instances with equal fields are NOT
 *      equal; two different functions are NOT equal. This is `a is b`, and it is
 *      why the array case above has to be explicit - otherwise arrays would fall
 *      through to identity and `[[1]] == [[1]]` would be False.
 *
 * Cross-type comparison is False rather than an error, which is why there is no
 * "Cannot compare" here: that text belongs to the ORDERING operators only. */
MXFN int mx_values_equal(MxValue a, MxValue b) {
    if (a.tag == MX_BOOL || b.tag == MX_BOOL) {
        /* only bool-equals-bool, and only when both ARE bools */
        return a.tag == MX_BOOL && b.tag == MX_BOOL && a.as.b == b.as.b;
    }
    if (a.tag == MX_NULL || b.tag == MX_NULL) {
        return a.tag == MX_NULL && b.tag == MX_NULL;
    }
    if (mx_is_num(a) && mx_is_num(b)) {
        /* Mixed int/float compares NUMERICALLY: 1 == 1.0 is true in the host,
         * because Python compares int to float by value. Comparing the tags
         * first would make 1 == 1.0 False, which is a divergence. */
        double x = (a.tag == MX_INT) ? (double)a.as.i : a.as.d;
        double y = (b.tag == MX_INT) ? (double)b.as.i : b.as.d;
        return x == y;
    }
    if (a.tag == MX_STR && b.tag == MX_STR) {
        if (!a.as.s || !b.as.s) return a.as.s == b.as.s;
        if (a.as.s == b.as.s) return 1;
        return strcmp(a.as.s, b.as.s) == 0;
    }
    if (a.tag == MX_ARR && b.tag == MX_ARR) {
        size_t i;
        if (a.as.arr->n != b.as.arr->n) return 0;
        for (i = 0; i < a.as.arr->n; i++) {
            if (!mx_values_equal(a.as.arr->v[i], b.as.arr->v[i])) return 0;
        }
        return 1;
    }
    /* identity, for the types that are compared by reference */
    if (a.tag == MX_FUNC && b.tag == MX_FUNC) return a.as.fn == b.as.fn;
    return 0;
}

/* is_truthy - interpreter.py:927-932, for `if`/`while` conditions and for the
 * short-circuit operators.
 *
 * The three tests are in the host's order and the order matters:
 *   null and false are falsy;
 *   then 0, "" and [] are falsy - note these are VALUE comparisons in Python
 *   (`v == 0` is true for `False` too, which is why the bool test came first);
 *   everything else is truthy, INCLUDING 0.0? No - `0.0 == 0` is True in Python,
 *   so 0.0 is falsy here too, and the C must match rather than reason about it.
 * An empty array is falsy, and that is checked by LENGTH, not by identity. */
static inline int mx_fast_truthy(MxValue v) {
    if (v.tag == MX_BOOL) return v.as.b ? 1 : 0;
    if (v.tag == MX_INT)  return v.as.i != 0;
    if (v.tag == MX_NULL) return 0;
    if (v.tag == MX_NUM)  return v.as.d != 0.0;
    if (v.tag == MX_STR)  return v.as.s[0] != '\0';
    if (v.tag == MX_ARR)  return v.as.arr->n != 0;
    return 1;              /* functions, instances, modules are truthy */
}

MXFN int mx_is_truthy(MxValue v) {
    return mx_fast_truthy(v);
}

/* mx_binop's comparison and equality arms live with it (below); the helpers
 * they need are mx_is_num and mx_type_name, both already defined above. */

MXFN MxValue mx_unary(const char *op, MxValue v) {
    if (strcmp(op, "MINUS") == 0 || strcmp(op, "-") == 0) {
        if (v.tag == MX_INT) return mx_int(-v.as.i);
        if (v.tag == MX_NUM) return mx_num(-v.as.d);
        if (v.tag == MX_BOOL)
            mx_raise("Cannot negate %s", mx_type_name(v));
        mx_raise("Cannot negate %s", mx_type_name(v));
        return mx_null();
    }
    if (strcmp(op, "NOT") == 0) {
        int t;
        if (v.tag != MX_BOOL) mx_raise("not expects a bool, got %s", mx_type_name(v));
        t = !v.as.b;
        return mx_bool(t);
    }
    mx_raise("Unknown unary op %s", op);
    return mx_null();
}

MXFN MxValue mx_binop(const char *op, MxValue l, MxValue r) {
    if (!op || !op[0]) {
        mx_raise("Unknown binop %s", op ? op : "");
        return mx_null();
    }
    if (op[0] == '+' && op[1] == '\0') {
        /* ARRAY CONCAT comes before the string branch, because the host tests
         * isinstance(x, list) first (:333): [] + [] is an array, and
         * [] + "s" would be a type error there, not a string. Getting this
         * order wrong silently turns array concat into string concat. */
        if (l.tag == MX_ARR && r.tag == MX_ARR) {
            MxValue out = mx_arr_value(mx_arr_new(l.as.arr->n + r.as.arr->n));
            size_t i;
            for (i = 0; i < l.as.arr->n; i++) out.as.arr->v[i] = l.as.arr->v[i];
            for (i = 0; i < r.as.arr->n; i++)
                out.as.arr->v[l.as.arr->n + i] = r.as.arr->v[i];
            out.as.arr->n = l.as.arr->n + r.as.arr->n;
            return out;
        }
        if (l.tag == MX_ARR || r.tag == MX_ARR) {
            /* one side is an array and the other is not: the host reaches
             * `raise RuntimeError(f"Cannot add {type_name(l)} and {type_name(r)}")`
             * here, because its list check needs BOTH sides. */
            mx_raise("Cannot add %s and %s", mx_type_name(l), mx_type_name(r));
        }
        if (l.tag == MX_STR || r.tag == MX_STR) {
            const char *sa = (l.tag == MX_STR) ? l.as.s : NULL;
            const char *sb = (r.tag == MX_STR) ? r.as.s : NULL;
            char bufa[512], bufb[512];
            if (!sa) {
                mx_display(l, bufa, sizeof bufa);
                sa = bufa;
            }
            if (!sb) {
                mx_display(r, bufb, sizeof bufb);
                sb = bufb;
            }
            size_t lena = strlen(sa);
            size_t lenb = strlen(sb);
            char *out = (char *)arena_alloc(lena + lenb + 1);
            memcpy(out, sa, lena);
            memcpy(out + lena, sb, lenb);
            out[lena + lenb] = '\0';
            return mx_str(out);
        }
        /* INTERP_DESIGN 3.2 footgun 2: the host tests bool FIRST, so
         * `true + 1` raises rather than becoming 2 (Python's bool-is-int). */
        if (mx_is_bool(l) || mx_is_bool(r))
            mx_raise("Cannot add %s and %s", mx_type_name(l), mx_type_name(r));
        if (mx_is_num(l) && mx_is_num(r)) {
            if (l.tag == MX_INT && r.tag == MX_INT) return mx_int(add_i64(l.as.i, r.as.i));
            return mx_num((l.tag == MX_INT ? (double)l.as.i : l.as.d) +
                          (r.tag == MX_INT ? (double)r.as.i : r.as.d));
        }
        mx_raise("Cannot add %s and %s", mx_type_name(l), mx_type_name(r));
    }
    if (op[0] == '=' && op[1] == '=') {
        return mx_bool(mx_values_equal(l, r));
    }
    if (op[0] == '!' && op[1] == '=') {
        return mx_bool(!mx_values_equal(l, r));
    }
    /* GT/LT/GTE/LTE */
    if (op[0] == '>' || op[0] == '<') {
        int is_gt = (op[0] == '>');
        int is_eq = (op[1] == '=');
        if (mx_is_num(l) && mx_is_num(r)) {
            double x = (l.tag == MX_INT) ? (double)l.as.i : l.as.d;
            double y = (r.tag == MX_INT) ? (double)r.as.i : r.as.d;
            if (is_gt) return mx_bool(is_eq ? (x >= y) : (x > y));
            return mx_bool(is_eq ? (x <= y) : (x < y));
        }
        if (l.tag == MX_STR && r.tag == MX_STR) {
            int c = strcmp(l.as.s, r.as.s);
            if (is_gt) return mx_bool(is_eq ? (c >= 0) : (c > 0));
            return mx_bool(is_eq ? (c <= 0) : (c < 0));
        }
        mx_raise("Cannot compare %s and %s", mx_type_name(l), mx_type_name(r));
    }
    if (op[0] == '-' && op[1] == '\0') {
        if (mx_is_bool(l) || mx_is_bool(r))
            mx_raise("Cannot subtract %s and %s", mx_type_name(l), mx_type_name(r));
        if (mx_is_num(l) && mx_is_num(r)) {
            if (l.tag == MX_INT && r.tag == MX_INT) return mx_int(sub_i64(l.as.i, r.as.i));
            return mx_num((l.tag == MX_INT ? (double)l.as.i : l.as.d) -
                          (r.tag == MX_INT ? (double)r.as.i : r.as.d));
        }
        mx_raise("Cannot subtract %s and %s", mx_type_name(l), mx_type_name(r));
    }
    if (op[0] == '*' && op[1] == '\0') {
        if (mx_is_num(l) && mx_is_num(r)) {
            if (l.tag == MX_INT && r.tag == MX_INT) return mx_int(mul_i64(l.as.i, r.as.i));
            return mx_num((l.tag == MX_INT ? (double)l.as.i : l.as.d) *
                          (r.tag == MX_INT ? (double)r.as.i : r.as.d));
        }
        mx_raise("Cannot multiply %s and %s", mx_type_name(l), mx_type_name(r));
    }
    if (op[0] == '/' && op[1] == '\0') {
        if (mx_is_num(l) && mx_is_num(r)) {
            double d = (r.tag == MX_INT ? (double)r.as.i : r.as.d);
            if (d == 0.0) mx_raise("Division by zero");
            return mx_num((l.tag == MX_INT ? (double)l.as.i : l.as.d) / d);
        }
        mx_raise("Cannot divide %s and %s", mx_type_name(l), mx_type_name(r));
    }
    if (op[0] == '%' && op[1] == '\0') {
        if (mx_is_bool(l) || mx_is_bool(r))
            mx_raise("Cannot modulo %s and %s", mx_type_name(l), mx_type_name(r));
        if (mx_is_num(l) && mx_is_num(r)) {
            if (l.tag == MX_INT && r.tag == MX_INT) {
                if (r.as.i == 0) mx_raise("Modulo by zero");
                return mx_int(l.as.i % r.as.i);
            }
            {   double y = (r.tag == MX_INT) ? (double)r.as.i : r.as.d;
                double f = fmod((l.tag == MX_INT) ? (double)l.as.i : l.as.d, y);
                return mx_num(f);
            }
        }
        mx_raise("Cannot modulo %s and %s", mx_type_name(l), mx_type_name(r));
    }
    mx_raise("Unknown binop %s", op);
    return mx_null();
}

/* ===================================================================== */
/* 5. The environment chain - SUBSTRATE (B-2 links against this).        */
/* ===================================================================== */
/* Moved out of the evaluator for B-2: a generated program creates envs but
 * never walks an AST, so the env must survive with -DMX_SUBSTRATE. */

typedef struct Binding { const char *name; MxValue v; } Binding;

typedef struct MxEnv {
    Binding *b;
    int n, cap;
    struct MxEnv *parent;
    Binding small_b[8];
} MxEnv;

typedef MxValue (*MxFnGen)(MxValue, MxValue, MxValue, MxValue, MxValue, MxValue, int);

/* ---- The COMPILED path's types (B-3.3) ----------------------------------
 *
 * Declared HERE, beside the interpreter's boxes, and UNGUARDED. Three reasons,
 * and the third is the one that forced the move:
 *   - display (section 4, ABOVE) reads an instance box's fields, so it needs
 *     these COMPLETE rather than forward-declared;
 *   - display's substrate half must agree with its interpreter half's TEXT, so
 *     the struct it reads may not vary with the build;
 *   - an unused TYPE costs nothing - -Wall does not warn about an unused struct
 *     - so an interpreter build carrying these declarations is strictly better
 *     than two builds whose display reads two different layouts.
 *
 * MxClassDesc is the shape a COMPILED class has: tables of field names and
 * initialisers, tables of method names and functions, and the init function in
 * its OWN slot (AGENTS.md's invariant - the same one the AST registry keeps).
 */
struct MxClassDesc_ {
    const char      *name;
    const char      *parent;      /* "" when there is none */
    const char     **field_names;
    MxValue         *field_vals;  /* evaluated at registration */
    int              nfields;
    const char     **method_names;
    MxFnGen         *method_fns;
    int              nmethods;
    MxFnGen          init_fn;     /* null when the class defines no init */
    int              init_arity;
    MxEnv           *mod_env;
    void            *defined;
};
typedef struct MxClassDesc_ MxClassDesc;


typedef struct { int64_t stamp; MxClassDesc *klass; MxDict *fields; } MxGenInst;
struct MxGenBound_ { int64_t stamp; MxFnGen fn; MxGenInst *inst;
                      MxClassDesc *klass; };
typedef struct MxGenBound_ MxGenBound;
struct MxGenSuper_ { MxGenInst *inst; MxClassDesc *klass; };
typedef struct MxGenSuper_ MxGenSuper;



/* The two NAME helpers display needs. They work for BOTH the interpreter's boxes
 * and the generated path's, because display lives in the SHARED region (it is the
 * substrate's output contract) while only the interpreter half has an MxNode.
 *
 * So each helper is compiled for the shapes its build mode can actually produce:
 * the interpreter build compiles the AST version, the substrate build compiles
 * the descriptor version. There is no runtime discrimination, because there is no
 * runtime ambiguity - a given binary only ever creates the boxes its half
 * defines. Reading the wrong union member would be undefined behaviour, and this
 * is the alternative to reading it.
 *
 * Both return the DEFINING class's name, because that is what a reader needs to
 * know about a bound method and it is what the host's `<method {name}>` (:85)
 * reports for an AST method. */
#ifdef MX_SUBSTRATE
/* OPAQUE forward declarations: these helpers read ONE field (`klass`, then
 * `name`), and a POINTER to an incomplete type is enough to do that. Defining
 * the boxes above display instead would have put MxEnv's dependencies (and
 * MxFnGen's) into the box section, which is where the ordering fights started. */

MXFN const char *mx_bound_method_name(MxValue v) {
    MxGenBound *bm = (MxGenBound *)v.as.box;
    return (bm && bm->klass) ? bm->klass->name : "?";
}
MXFN const char *mx_super_class_name(MxValue v) {
    MxGenSuper *sp = (MxGenSuper *)v.as.box;
    return (sp && sp->klass) ? sp->klass->name : "?";
}
/* Both helpers dispatch on the TAG, which is the whole reason MX_GENINST
 * exists: a B-2 binary holds both box layouts, and the tag is what says which
 * one a given value carries. The rendered TEXT is the same either way - that is
 * what the corpus compares - so this changes no golden. */
MXFN const char *mx_inst_class_name(MxValue v) {
#ifdef MX_BOTH
    /* An AST instance only exists in a binary that also has the AST half, so
     * this arm is compiled only there - the pure substrate build has no
     * mx_class_name_of to call, and naming it unconditionally was an implicit
     * declaration plus an int-to-pointer return. */
    if (v.tag == MX_INST)
        return mx_class_name_of(((MxInstance *)v.as.box)->klass);
#endif
    {   MxGenInst *gi = (MxGenInst *)v.as.box;
        return (gi && gi->klass) ? gi->klass->name : "?";
    }
}
MXFN MxDict *mx_inst_fields(MxValue v) {
#ifdef MX_BOTH
    if (v.tag == MX_INST) return ((MxInstance *)v.as.box)->fields;
#endif
    {   MxGenInst *gi = (MxGenInst *)v.as.box;
        return gi ? gi->fields : NULL;
    }
}
#else
MXFN const char *mx_bound_method_name(MxValue v) {
    MxBoundMethod *bm = (MxBoundMethod *)v.as.box;
    return bm ? mx_class_name_of(bm->method) : "?";
}
MXFN const char *mx_super_class_name(MxValue v) {
    MxSuper *sp = (MxSuper *)v.as.box;
    return (sp && sp->klass) ? mx_class_name_of(sp->klass) : "?";
}
#endif

/* g_globals - the program's global scope. EXTERNAL under MX_BOTH (the AST half
 * defines it, because the import walk and the evaluator both need one globals and
 * two would be two scopes) and this file's own otherwise. Declared HERE because
 * the generated path reads it too: mx_new_multi's dotted-name lookup resolves
 * `mod.Klass` through g_globals exactly as eval_new does, and that lookup sits
 * above the AST half's definition. */
MxEnv *g_globals = NULL;



static MxEnv *g_env_freelist = NULL;

MXFN MxEnv *env_new(MxEnv *parent) {
    MxEnv *volatile e;
    if (g_env_freelist) {
        e = g_env_freelist;
        g_env_freelist = e->parent;
        e->parent = parent;
        e->n = 0;
        e->cap = 8;
        e->b = e->small_b;
        return e;
    }
    e = (MxEnv *)arena_alloc(sizeof(MxEnv));
    e->parent = parent; e->n = 0; e->cap = 8;
    e->b = e->small_b;
    return e;
}

MXFN void env_free(MxEnv *e) {
    if (!e || e == g_globals) return;
    e->cap = 8;
    e->b = e->small_b;
    e->parent = g_env_freelist;
    g_env_freelist = e;
}
#define GLOBALS_HASH_SIZE 1024
typedef struct MxGlobalBucket {
    const char *name;
    MxValue v;
    struct MxGlobalBucket *next;
} MxGlobalBucket;

static MxGlobalBucket *g_globals_map[GLOBALS_HASH_SIZE];

MXFN void env_define(MxEnv *e, const char *name, MxValue v) {
    name = mx_fast_intern(name);
    if (e == g_globals) {
        size_t b = (((uintptr_t)name) >> 3) & (GLOBALS_HASH_SIZE - 1);
        MxGlobalBucket *gb;
        for (gb = g_globals_map[b]; gb; gb = gb->next) {
            if (gb->name == name) {
                gb->v = v;
                int i;
                for (i = 0; i < e->n; i++) {
                    if (e->b[i].name == name) { e->b[i].v = v; return; }
                }
                return;
            }
        }
        MxGlobalBucket *ngb = (MxGlobalBucket *)arena_alloc(sizeof(MxGlobalBucket));
        ngb->name = name;
        ngb->v = v;
        ngb->next = g_globals_map[b];
        g_globals_map[b] = ngb;
        if (e->n >= e->cap) {
            int cap = e->cap * 2;
            Binding *nb = (Binding *)arena_alloc((size_t)cap * sizeof(Binding));
            memcpy(nb, e->b, (size_t)e->n * sizeof(Binding));
            e->b = nb; e->cap = cap;
        }
        e->b[e->n].name = name; e->b[e->n].v = v; e->n++;
        return;
    }
    int i;
    for (i = 0; i < e->n; i++)
        if (e->b[i].name == name) { e->b[i].v = v; return; }
    if (e->n >= e->cap) {
        int cap = e->cap * 2;
        Binding *nb = (Binding *)arena_alloc((size_t)cap * sizeof(Binding));
        memcpy(nb, e->b, (size_t)e->n * sizeof(Binding));
        e->b = nb; e->cap = cap;
    }
    e->b[e->n].name = name; e->b[e->n].v = v; e->n++;
}

MXFN int env_get(MxEnv *e, const char *name, MxValue *out) {
    if (!e || !name) return 0;
    const char *iname = mx_fast_intern(name);
    MxEnv *cur;
    int i;
    for (cur = e; cur; cur = cur->parent) {
        if (cur == g_globals) {
            size_t b = (((uintptr_t)iname) >> 3) & (GLOBALS_HASH_SIZE - 1);
            MxGlobalBucket *gb;
            for (gb = g_globals_map[b]; gb; gb = gb->next) {
                if (gb->name == iname) { *out = gb->v; return 1; }
            }
            break;
        }
        for (i = 0; i < cur->n; i++)
            if (cur->b[i].name == iname) { *out = cur->b[i].v; return 1; }
    }
    return 0;
}

/* The scoping rule (COMPILE_DESIGN §2.3, interpreter.py:139): a total miss
 * DEFINES in the innermost env. INTERP_DESIGN 4.1 records this as I-M1's first
 * required assertion - it is invisible until it is missing. */
MXFN void env_set(MxEnv *e, const char *name, MxValue v) {
    if (!e || !name) return;
    const char *iname = mx_fast_intern(name);
    MxEnv *cur;
    int i;
    for (cur = e; cur; cur = cur->parent) {
        if (cur == g_globals) {
            size_t b = (((uintptr_t)iname) >> 3) & (GLOBALS_HASH_SIZE - 1);
            MxGlobalBucket *gb;
            for (gb = g_globals_map[b]; gb; gb = gb->next) {
                if (gb->name == iname) {
                    gb->v = v;
                    for (i = 0; i < cur->n; i++) {
                        if (cur->b[i].name == iname) { cur->b[i].v = v; return; }
                    }
                    return;
                }
            }
            break;
        }
        for (i = 0; i < cur->n; i++)
            if (cur->b[i].name == iname) { cur->b[i].v = v; return; }
    }
    env_define(e, iname, v);
}


#ifdef MX_SUBSTRATE
MXFN void mx_store_index(MxEnv *env, const char *name, MxValue idx, MxValue val) {
    MxValue arr;
    if (!env_get(env, name, &arr)) mx_raise("Undefined variable '%s'", name);
    arr = mx_index_set(arr, idx, val);
    env_set(env, name, arr);
}
#endif /* MX_SUBSTRATE */

#ifdef MX_SUBSTRATE
/* ===========================================================================
 * THE GENERATED PATH'S RUNTIME (B-3.3)
 *
 * Everything above is shared: values, dicts, environments, display, binops, the
 * catch machinery. This section is the half that GENERATED code calls - the
 * registry of compiled classes, and the entry points the emitter names.
 *
 * The compiled path reuses the interpreter's TAGS (MX_INST, MX_METHOD, ...) and
 * its box union, because display (shared, above) switches on the tag. It cannot
 * reuse the interpreter's BOX STRUCTURES, because those hold MxNode pointers to
 * an AST that does not exist here - which is why display has an MX_SUBSTRATE
 * half reading MxGenInst/MxGenBound instead. One tag per kind, two possible
 * layouts, selected at BUILD time and never at run time.
 * =========================================================================== */

/* The registry: growable, ordered, last-registration-wins by NAME - the same
 * rule the AST registry uses (mx_define_class, :1567). A duplicate KEEPS ITS
 * ORIGINAL SLOT, so growth order is stable and a lookup that stops at the
 * first name match still sees the newest definition. */
static MxClassDesc **g_genclasses = NULL;
static size_t         g_ngen = 0, g_capgen = 0;

MXFN void mx_register_class(MxClassDesc *d) {
    if (!d) return;
    d->name = mx_fast_intern(d->name);
    if (d->parent && d->parent[0]) d->parent = mx_fast_intern(d->parent);
    int j;
    for (j = 0; j < d->nfields; j++) {
        d->field_names[j] = mx_fast_intern(d->field_names[j]);
    }
    for (j = 0; j < d->nmethods; j++) {
        d->method_names[j] = mx_fast_intern(d->method_names[j]);
    }
    size_t i;
    for (i = 0; i < g_ngen; i++)
        if (g_genclasses[i]->name == d->name) {
            g_genclasses[i] = d;
            return;
        }
    if (g_ngen >= g_capgen) {
        size_t cap = g_capgen ? g_capgen * 2 : 8;
        MxClassDesc **nb = (MxClassDesc **)arena_alloc(cap * sizeof(MxClassDesc *));
        if (g_genclasses) memcpy(nb, g_genclasses, g_ngen * sizeof(MxClassDesc *));
        g_genclasses = nb; g_capgen = cap;
    }
    g_genclasses[g_ngen++] = d;
}

#ifdef MX_BOTH
/* mx_new_module_class is DEFINED below, in the eval_dot section, because it is a
 * module operation and belongs with the other three. Declared here because
 * mx_new_multi's dotted-name lookup calls it, and that lookup is above. */
static MxValue mx_new_module_class(MxModule *mod, const char *name,
                                   MxValue *args, int nargs);
/* ast_new_by_name - build an instance of an AST-REGISTERED class, by name.
 *
 * A generated `newx Dict()` asks the GENERATED registry for a descriptor, and a
 * class that arrived through an IMPORT is in the AST registry instead - the
 * module was read at run time, never emitted. So the two registries must be
 * tried in turn, and this is the AST half of that.
 *
 * It builds the node mx_eval_new would have built and hands it to mx_eval_new,
 * rather than repeating the three steps: merged fields parent-first, the nearest
 * init up the chain, arguments to the init. A second copy of the merge rule is a
 * second answer to the same question. */
static MxValue ast_new_by_name(const char *class_name, MxValue *args, int nargs);
/* The AST half's own DotAccess, and its instance-field WRITE. Declared here
 * because the generated path calls them: in a B-2 binary an instance can be
 * EITHER layout, and which one is a property of the value (its tag), not of the
 * build. The generated path therefore hands an AST instance straight back to the
 * half that owns it - which is why a module class behaves identically here and
 * when it is the program. */
static MxValue mx_eval_dot_ast(MxValue obj, const char *field);
static void    mx_ast_set_field(MxValue obj, const char *field, MxValue val);
static MxValue mx_ast_call_method(MxValue recv, const char *name,
                                  MxValue *args, int nargs);
#endif

/* The generated-FUNCTION registry, the same shape and the same rule.
 *
 * It exists because mx_call_list gets a NAME, not a pointer - and it has to be
 * a name, because the emitter writes the call as a C expression naming a string
 * literal. A pointer would make the emitter look the function up too, which puts
 * the lookup in every call expression instead of once.
 *
 * "Last registration wins" is the host's rule for module functions (AGENTS.md:
 * module function names are global, last import wins), so the emitter registers
 * in source order and a later definition of the same name replaces the earlier
 * one. */
#define GENFN_HASH_SIZE 512
typedef struct MxGenFnBucket {
    const char *name;
    MxFnGen fn;
    struct MxGenFnBucket *next;
} MxGenFnBucket;

static MxGenFnBucket *g_genfn_map[GENFN_HASH_SIZE];

MXFN void mx_register_genfn(const char *name, MxFnGen fn) {
    name = mx_fast_intern(name);
    size_t b = (((uintptr_t)name) >> 3) & (GENFN_HASH_SIZE - 1);
    MxGenFnBucket *bucket;
    for (bucket = g_genfn_map[b]; bucket; bucket = bucket->next) {
        if (bucket->name == name) {
            bucket->fn = fn;
            return;
        }
    }
    MxGenFnBucket *nb = (MxGenFnBucket *)arena_alloc(sizeof(MxGenFnBucket));
    nb->name = name;
    nb->fn = fn;
    nb->next = g_genfn_map[b];
    g_genfn_map[b] = nb;
}

/* Two forward declarations, for the same reason C needs them at all: the
 * definitions below are in a section that reads better in this order. */
MXFN MxClassDesc *class_chain_parent(MxClassDesc *k);
MXFN MxValue mx_inst_value(MxGenInst *inst);
/* mx_call_fn_ptr is DEFINED in the last section (the calling convention), which
 * is after this one. A declaration here, not a move: the definition belongs with
 * the convention it implements, and moving it would separate the argument-count
 * check from the argument list it checks. */
MXFN MxValue mx_call_fn_ptr(MxFnGen fn, MxValue p0, MxValue p1, MxValue p2,
                            MxValue p3, MxValue p4, MxValue p5, int nargs);

MXFN MxFnGen gen_fn_lookup(const char *name) {
    if (!name) return NULL;
    const char *iname = mx_fast_intern(name);
    size_t b = (((uintptr_t)iname) >> 3) & (GENFN_HASH_SIZE - 1);
    MxGenFnBucket *bucket;
    for (bucket = g_genfn_map[b]; bucket; bucket = bucket->next) {
        if (bucket->name == iname) return bucket->fn;
    }
    return NULL;
}

/* gen_class_of - the descriptor for a name, or NULL. A miss is not an error
 * HERE: every caller decides, because `newx` says "no class 'X'" while a
 * DotAccess on a module says "module 'X' has no field". */
MXFN MxClassDesc *gen_class_of(const char *name) {
    if (!name) return NULL;
    const char *iname = mx_fast_intern(name);
    size_t i;
    for (i = g_ngen; i > 0; i--)
        if (g_genclasses[i - 1]->name == iname) return g_genclasses[i - 1];
    return NULL;
}

/* find_method_near - the nearest definition of `name` at or above `klass`.
 *
 * Walks UP the chain, which is what makes an inherited method work: a subclass
 * that defines no `speak` finds its parent's. The DESCRIPTOR is returned as
 * well as the function, because a caller that binds needs both - and the
 * defining class is what `super` is measured from (I-M4: super's klass is the
 * class DEFINING the current method, not the receiver's class, and that is why
 * a multi-level chain resolves correctly). */
static int find_method_near(MxClassDesc *klass, const char *name,
                            MxFnGen *out_fn, MxClassDesc **out_klass) {
    MxClassDesc *k;
    const char *iname = mx_fast_intern(name);
    for (k = klass; k; k = class_chain_parent(k)) {
        int i;
        /* init is in its OWN slot (AGENTS.md's invariant), so a table scan MISSES
         * it - and `super.init(n)` is exactly the call the corpus makes. The
         * descriptor keeps it out of method_names on purpose, so the lookup has
         * to know about the separate slot, the same way the host's find_method
         * does (`if name == "init" and klass.get("init")`). */
        if (name[0] == 'i' && strcmp(name, "init") == 0 && k->init_fn) {
            *out_fn = k->init_fn;
            *out_klass = k;
            return 1;
        }
        for (i = 0; i < k->nmethods; i++)
            if (k->method_names[i] == iname) {
                *out_fn = k->method_fns[i];
                *out_klass = k;
                return 1;
            }
    }
    return 0;
}

/* The receiver and the DEFINING class, live for the duration of one call.
 *
 * Two globals rather than two hidden parameters because the generated method
 * body reads `this` and `super` by NAME - they are ordinary expressions in the
 * emitted source, not a convention the emitter has to thread through every
 * nested call expression. The save/restore in mx_call_instance_gen is what
 * makes them correct under RECURSION: a method that calls another method gets
 * its own receiver back when the inner call returns. Leaving them unrestored
 * is the bug this pairing prevents - a `super` two frames down would otherwise
 * resolve against the outer call's receiver. */
static MxGenInst   *g_cur_inst = NULL;
static MxClassDesc *g_cur_class = NULL;

/* mx_call_instance_gen - call a generated METHOD with a receiver.
 *
 * The receiver goes in slot 0 and the arguments follow, so a method's prologue
 * binds "this" from p0_ and its parameters from p1_ upward (the emitter's
 * emit_method). `defining` is the class whose table the function came from,
 * which is NOT the receiver's class - see above. */
MXFN MxValue mx_call_instance_gen(MxFnGen fn, MxGenInst *inst, MxValue *args,
                                  int nargs, MxClassDesc *defining) {
    MxValue p[MX_MAX_PARAMS], r;
    MxGenInst *si = g_cur_inst;
    MxClassDesc *sc = g_cur_class;
    int i;
    if (nargs > MX_MAX_PARAMS - 1)
        mx_raise("method has %d arguments, more than the %d the substrate passes",
                 nargs, MX_MAX_PARAMS - 1);
    p[0] = mx_inst_value(inst);   /* tagged MX_GENINST, which is what a method's
                                   * `this` must be for the generated lookup */
    for (i = 0; i < nargs; i++) p[i + 1] = args[i];
    g_cur_inst = inst;
    g_cur_class = defining;
    /* An init's own ENV is the class's module env, stamped at REGISTRATION
     * (mx_register_class) rather than baked into the code, so a class defined
     * inside a module still sees that module's lets. */
    r = fn(p[0], p[1], p[2], p[3], p[4], p[5], nargs);
    g_cur_inst = si;
    g_cur_class = sc;
    return r;
}

/* mx_inst_value - box a generated instance. A function, because every path that
 * creates an instance needs it and inlining the three lines would put the tag
 * next to the box twice. */
MXFN MxValue mx_inst_value(MxGenInst *inst) {
    MxValue v;
    v.tag = MX_GENINST; v.as.box = inst;
    return v;
}

/* class_chain_parent - the parent DESCRIPTOR, resolved by name. NULL for a
 * class with no parent, which terminates every chain walk. The name is looked up
 * in the registry rather than stored as a pointer because a parent may be
 * defined AFTER the child that names it - and because the registry is mutable,
 * a forward reference must not freeze the wrong answer into the descriptor. */
MXFN MxClassDesc *class_chain_parent(MxClassDesc *k) {
    if (!k || !k->parent || !k->parent[0]) return NULL;
    return gen_class_of(k->parent);
}

/* mx_dot - `obj.field` on the COMPILED path.
 *
 * The order is the host's, and it is the one non-obvious rule in this section
 * (I-M4, eval_dot): FIELDS BEFORE METHODS. A field may SHADOW a method of the
 * same name, because the field lookup returns on the first hit. Getting this
 * backwards is silent - it produces a callable where a value was expected, or
 * the reverse - which is why the comment states the rule rather than restating
 * the code.
 *
 * Modules are the second arm, and they keep the host's THREE independent slots in
 * the host's order: funcs, then classes, then lets. Each is searched on its own,
 * so a module's function `x` and its `let x` coexist and the function wins. */
MXFN int mx_try_get_field(MxValue obj, const char *field, MxValue *out) {
    if (obj.tag == MX_GENINST) {
        MxGenInst *inst = (MxGenInst *)obj.as.box;
        if (inst && inst->fields && mx_dict_lookup(inst->fields, field, out)) {
            return 1;
        }
    }
#ifdef MX_BOTH
    if (obj.tag == MX_INST) {
        MxInstance *inst = (MxInstance *)obj.as.box;
        if (inst && inst->fields && mx_dict_lookup(inst->fields, field, out)) {
            return 1;
        }
    }
#endif
    return 0;
}

MXFN MxValue mx_dot(MxValue obj, const char *field) {
    MxValue v;
#ifdef MX_BOTH
    /* An AST instance in a B-2 binary is the OTHER layout, and its lookup is the
     * AST half's own eval_dot (fields first, then the chain for a method, with
     * the host's two texts). Delegating rather than reimplementing is the point:
     * a module class's instance must print and behave exactly as it does when
     * the module was the program. */
    if (obj.tag == MX_INST)
        return mx_eval_dot_ast(obj, field);
#endif
    if (obj.tag == MX_GENINST) {
        MxGenInst *inst = (MxGenInst *)obj.as.box;
        /* 1. the instance's OWN fields - where `obj.x = v` puts the value. */
        if (inst->fields && mx_dict_lookup(inst->fields, field, &v)) {
            return v;
        }
        /* 2. the CLASS chain's DECLARED fields, walked up parent-first with the
         * child overriding - the merge rule from AGENTS.md's inheritance
         * invariant. Separate from the dictionary because a declared field with
         * no assigned value yet is still readable. */
        {
            MxClassDesc *k;
            const char *ifield = mx_fast_intern(field);
            for (k = inst->klass; k; k = class_chain_parent(k)) {
                int i;
                for (i = 0; i < k->nfields; i++)
                    if (k->field_names[i] == ifield)
                        return k->field_vals[i];
            }
        }
        /* 3. a METHOD, bound. */
        {
            MxFnGen fn; MxClassDesc *def;
            if (find_method_near(inst->klass, field, &fn, &def)) {
                MxGenBound *bm = (MxGenBound *)arena_alloc(sizeof(MxGenBound));
                bm->stamp = mx_next_stamp();
                bm->fn = fn; bm->inst = inst; bm->klass = def;
                v.tag = MX_METHOD; v.as.box = bm;
                return v;
            }
        }
        mx_raise("Field/method '%s' not found on %s", field,
                 inst->klass ? inst->klass->name : "?");
        return mx_null();   /* unreachable: mx_raise does not return */
    }
    if (obj.tag == MX_SUPER) {
        /* A super box's dot is a method on the PARENT of the class defining the
         * running method - and NEVER an instance field, which is the whole point
         * of super (AGENTS.md: super does not look up fields). */
        MxGenSuper *sp = (MxGenSuper *)obj.as.box;
        MxFnGen fn; MxClassDesc *def;
        if (find_method_near(sp->klass, field, &fn, &def)) {
            MxGenBound *bm = (MxGenBound *)arena_alloc(sizeof(MxGenBound));
            bm->stamp = mx_next_stamp();
            bm->fn = fn; bm->inst = sp->inst; bm->klass = def;
            v.tag = MX_METHOD; v.as.box = bm;
            return v;
        }
        mx_raise("super has no method '%s'", field);
    }
    if (obj.tag == MX_METHOD) {
        /* Re-reading a field off a bound method: the host returns the method
         * itself when the name is in the defining class's table, so `m.m` is
         * not an error. A name that is NOT in the table is. */
        MxGenBound *bm = (MxGenBound *)obj.as.box;
        if (bm && bm->klass) {
            int i;
            for (i = 0; i < bm->klass->nmethods; i++)
                if (strcmp(bm->klass->method_names[i], field) == 0) return obj;
        }
        mx_raise("no field '%s' on '%s'", field, mx_bound_method_name(obj));
    }
    if (obj.tag == MX_MOD) {
        MxModule *mod = (MxModule *)obj.as.box;
        /* mx_dict_get returns null on a miss, so each slot is ASKED first: a
         * stored null would otherwise be indistinguishable from an absent key,
         * and the host distinguishes them. */
        if (mx_dict_has(mod->funcs, field))   return mx_dict_get(mod->funcs, field);
        if (mx_dict_has(mod->classes, field)) return mx_dict_get(mod->classes, field);
        if (mx_dict_has(mod->lets, field))    return mx_dict_get(mod->lets, field);
        mx_raise("Module '%s' has no attribute '%s'", mod->mod_name, field);
    }
    if (obj.tag == MX_STR)
        mx_raise("Cannot access field '%s' on string", field);
    mx_raise("Cannot access field '%s' on %s", field, mx_type_name(obj));
    /* unreachable: mx_raise does not return. Written because mx_raise is not
     * _Noreturn, and the string-typed arm above is a one-line `if` with no
     * braces - so this is the only statement that closes the function. */
    return mx_null();
}

/* mx_new_multi - `newx Klass(args...)`.
 *
 * TWO steps, and this is the host's rule exactly (interpreter.py:653-666):
 *   1. the class chain's fields are merged PARENT-FIRST (child overrides) into a
 *      dictionary, each value evaluated at REGISTRATION time (the emitter puts
 *      those evaluations in the class's register function);
 *   2. the NEAREST init up the chain is called with the arguments.
 *
 * The step I had wrong, and the reason this comment exists: the arguments are
 * NOT bound to the fields. `newx Animal("Rex")` passes "Rex" to `init`, and it is
 * the init's own body that decides what to do with it - here `this.name = n`.
 * Binding argument 0 to field 0 would compile and then print the wrong thing:
 * the corpus's Animal declares `legs` FIRST and `name` second, so a positional
 * binding puts "Rex" in `legs` and leaves `name` at "generic". A positional
 * binding is also wrong for a class whose init takes three parameters, because
 * there are no fields to bind them to at all. */
MXFN MxValue mx_new_multi(const char *class_name, MxValue *args, int nargs) {
    MxClassDesc *k;
#ifdef MX_BOTH
    /* A DOTTED name: `newx mod.Klass(...)`. The host splits on the dot, looks the
     * MODULE up in globals and takes the class out of mod.classes
     * (interpreter.py:639-645) - it does NOT look the whole "mod.Klass" string up
     * in the class registry, so the generated registry cannot answer it either.
     * So this defers to the same path a module class always takes, and the
     * B-2-only build (no AST half) reports the miss with the HOST's text rather
     * than inventing a third spelling of "no such class". */
    {   const char *dot = strrchr(class_name, '.');
        if (dot) {
            char modname[256];
            size_t n = (size_t)(dot - class_name);
            MxValue modv;
            if (n >= sizeof modname) mx_raise("Class '%s' not defined", class_name);
            memcpy(modname, class_name, n);
            modname[n] = '\0';
            if (env_get(g_globals, modname, &modv) && modv.tag == MX_MOD)
                return mx_new_module_class((MxModule *)modv.as.box, dot + 1,
                                           args, nargs);
            mx_raise("Module '%s' has no class '%s'", modname, dot + 1);
        }
    }
#endif
    k = gen_class_of(class_name);
#ifdef MX_BOTH
    /* A miss here is not a missing class - it is a class the AST half owns.
     * The compiled interpreter builds instances of stdlib classes (interp.mx's
     * `newx Dict()`), and every one of them arrives at this line. */
    if (!k) return ast_new_by_name(class_name, args, nargs);
#endif
    MxGenInst *inst;
    MxDict  *fields;
    MxValue  out;
    int      i;
    if (!k) mx_raise("Class '%s' not defined", class_name);
    fields = mx_dict_new(8);
    inst = (MxGenInst *)arena_alloc(sizeof(MxGenInst));
    inst->stamp = mx_next_stamp();
    inst->klass = k;
    inst->fields = fields;
    /* 1. parent-first merge. Walking child-first and stopping at the first hit
     * would let a child's declaration hide the parent's - the merge rule
     * inverted. */
    {
        MxClassDesc *c;
        for (c = k; c; c = class_chain_parent(c))
            for (i = 0; i < c->nfields; i++)
                if (!mx_dict_has(fields, c->field_names[i]))
                    mx_dict_set(fields, c->field_names[i], c->field_vals[i]);
    }
    out.tag = MX_GENINST; out.as.box = inst;
    /* 2. the nearest init up the chain, with the arguments. An inherited init
     * applies when the subclass defines none (AGENTS.md). */
    {
        MxClassDesc *c;
        for (c = k; c; c = class_chain_parent(c))
            if (c->init_fn) { mx_call_instance_gen(c->init_fn, inst, args, nargs, c); break; }
    }
    return out;
}

/* mx_new - the SHORT form the emitter emits for `newx Klass` with at most one
 * argument. It delegates rather than repeating the three steps above, for the
 * same reason the emitter has exactly one argument padder: two copies of a
 * three-step rule is two chances for one of them to be wrong. */
MXFN MxValue mx_new(const char *class_name, MxValue a0, int nargs) {
    MxValue args[MX_MAX_PARAMS];
    int i;
    for (i = 0; i < nargs && i < MX_MAX_PARAMS; i++) args[i] = a0;
    return mx_new_multi(class_name, args, nargs);
}

/* mx_set_field - the WRITE half of mx_dot's first arm, the pair of the emitted
 * `this.name = n`.
 *
 * It writes the instance's OWN dictionary, and refuses anything else: the host's
 * error is "Cannot set field 'x' on int", and a store that quietly created a
 * dictionary on a number would be a wrong answer rather than a failure. It
 * returns the value so the emitted statement stays a single expression. */
MXFN void mx_set_field(MxValue obj, const char *field, MxValue val) {
#ifdef MX_BOTH
    /* An AST instance belongs to the AST half, so its WRITE goes there too -
     * delegating a store to a reader would compile and silently do nothing. */
    if (obj.tag == MX_INST) { mx_ast_set_field(obj, field, val); return; }
#endif
    if (obj.tag != MX_GENINST)
        mx_raise("Cannot set field '%s' on %s", field, mx_type_name(obj));
    if (!((MxGenInst *)obj.as.box)->fields)
        ((MxGenInst *)obj.as.box)->fields = mx_dict_new(8);
    mx_dict_set(((MxGenInst *)obj.as.box)->fields, field, val);
}

/* mx_super_box - the `super` VALUE inside a method.
 *
 * Reads the receiver and the defining class from the two globals that
 * mx_call_instance_gen saves and restores around every call. The defining class is
 * NOT the receiver's class - that is I-M4's rule and the reason a multi-level
 * chain resolves: super() inside Bird.speak must reach Animal.speak even when the
 * receiver's own class is something else, because the running method was DEFINED
 * on Bird. So `super` is a property of the running METHOD, not of the object. */
MXFN MxValue mx_super_box(void) {
    MxGenSuper *sp;
    MxValue v;
    if (!g_cur_inst)
        mx_raise("super is only valid inside a method");
    sp = (MxGenSuper *)arena_alloc(sizeof(MxGenSuper));
    sp->inst = g_cur_inst;
    sp->klass = class_chain_parent(g_cur_class);
    v.tag = MX_SUPER; v.as.box = sp;
    return v;
}
#ifdef MX_BOTH
/* Three forward declarations for the MODULE arm of mx_call_list below.
 *
 * They are declarations rather than moves because their definitions live in the
 * AST half (mx_call_fn_list, mx_class_chain) and mx_call_list sits ABOVE that
 * half. Declaring a function whose definition is further down is ordinary C;
 * what is NOT ordinary is guessing the signature, so each one repeats the
 * definition's parameter list exactly.
 *
 * Guarded by MX_BOTH because these are the AST half's functions: a pure
 * substrate build has no mx_class_chain to declare, and a declaration of a
 * function that build will never define is exactly the -Wunused-function G6
 * refuses. */
static MxValue mx_call_fn_list(MxNode *fn, MxValue *args, int nargs,
                               MxEnv *globals);
static MxValue mx_new_module_class(MxModule *mod, const char *name,
                                   MxValue *args, int nargs);
static int     mx_class_chain(MxNode *klass, MxNode **out);
static MxValue mx_call_instance(MxNode *fn, MxValue *args, int nargs,
                                MxInstance *inst, MxNode *defining);
static MxValue mx_new_module_class(MxModule *mod, const char *name,
                                   MxValue *args, int nargs);
/* The interpreter half's globals, and its evaluator. A B-2 binary carries both
 * halves (MX_BOTH) because a module imported at run time is SOURCE, so these
 * exist and are used - which is why declaring them here is not dead weight. */
extern MxEnv  *g_globals;
static MxValue mx_eval(MxNode *n, MxEnv *env);
/* MX_CHAIN_MAX is the class-chain bound (a chain longer than this is refused
 * rather than walked off the end). It is defined with the AST half's limits, so
 * a forward use of it here needs the number, not the name. */
/* MX_CHAIN_MAX (the AST half's class-chain bound) is defined further down,
 * with the AST half's other limits, and a forward use of it here needs the
 * NUMBER rather than the name. 64 is the same bound, and the C11
 * _Static_assert below is what stops the two copies drifting: if the
 * other definition ever changes, this build FAILS to compile instead of
 * quietly sizing one buffer differently from the other. */
#endif /* MX_BOTH */

/* mx_call_list - the ONE call the emitter emits, for a free function and for a
 * method alike.
 *
 * A name alone cannot decide which: a method takes a receiver and a generated
 * function does not. So BOTH the name and the receiver go into one call rather
 * than giving the emitter two functions that could drift apart. A free function
 * passes mx_null() as the receiver, which cannot be confused with a real one
 * because no MountX value is null where an instance is expected - an instance is
 * a boxed MX_INST, never a null tag. */
MXFN MxValue mx_call_list(const char *name, MxValue recv,
                          MxValue p0, MxValue p1, MxValue p2, MxValue p3,
                          MxValue p4, MxValue p5, int nargs) {
    MxValue args[MX_MAX_PARAMS];
    if (nargs > MX_MAX_PARAMS)
        mx_raise("call has %d arguments, more than the %d the substrate passes",
                 nargs, MX_MAX_PARAMS);
    args[0] = p0; args[1] = p1; args[2] = p2;
    args[3] = p3; args[4] = p4; args[5] = p5;
    if (recv.tag != MX_NULL) {
        /* a METHOD call. THREE receiver shapes, because `a.m()` (an instance),
         * `super.m()` (a super box) and `let f = a.m; f()` (an already-bound
         * method) are three different lookups, not one with a flag. */
        if (recv.tag == MX_METHOD) {
            MxGenBound *bm = (MxGenBound *)recv.as.box;
            return mx_call_instance_gen(bm->fn, bm->inst, args, nargs, bm->klass);
        }
#ifdef MX_BOTH
        /* An AST instance in a B-2 binary: the method is an MxNode in the
         * receiver's class, and the AST half's own call machinery runs it. That
         * is the same path mx_eval_dot would take, so a module class's method
         * behaves identically here and when the module was the program. */
        if (recv.tag == MX_INST)
            return mx_ast_call_method(recv, name, args, nargs);
#endif
        if (recv.tag == MX_GENINST) {
            MxFnGen fn; MxClassDesc *def;
            if (!find_method_near(((MxGenInst *)recv.as.box)->klass, name, &fn, &def))
                mx_raise("no method '%s' on instance of '%s'", name,
                         ((MxGenInst *)recv.as.box)->klass
                             ? ((MxGenInst *)recv.as.box)->klass->name : "?");
            return mx_call_instance_gen(fn, (MxGenInst *)recv.as.box, args, nargs, def);
        }
        if (recv.tag == MX_SUPER) {
            MxGenSuper *sp = (MxGenSuper *)recv.as.box;
            MxFnGen fn; MxClassDesc *def;
            if (!find_method_near(sp->klass, name, &fn, &def))
                mx_raise("super has no method '%s'", name);
            return mx_call_instance_gen(fn, sp->inst, args, nargs, def);
        }
#ifdef MX_BOTH
        if (recv.tag == MX_MOD) {
            /* `mod.fn(...)` - a MODULE function, which is an AST FuncDef (a
             * module is read at run time, not compiled, so its functions are
             * walked rather than generated). So this arm cannot use MxFnGen: it
             * hands the node to the interpreter half's own calling convention,
             * which is the one that knows how to run a FuncDef.
             *
             * The funcs slot is searched BEFORE classes and lets, exactly as
             * mx_dot does, so `mod.x(1)` and `mod.x` agree on which x is meant -
             * a call that searched differently from the read would be a value and
             * a call that disagree. */
            MxModule *mod = (MxModule *)recv.as.box;
            MxValue fv;
            if (mx_dict_lookup(mod->funcs, name, &fv)) {
                return mx_call_fn_list((MxNode *)fv.as.box, args, nargs,
                                       g_globals);
            }
            /* a module CLASS, called as a constructor: `newx mod.Box()` is a
             * New node, but `mod.Box(...)` reads as a call, and the host's eval
             * treats a class name in call position the same way. */
            if (mx_dict_has(mod->classes, name))
                return mx_new_module_class(mod, name, args, nargs);
            mx_raise("Module '%s' has no attribute '%s'", mod->mod_name, name);
        }
#endif /* MX_BOTH - a module is SOURCE, so calling into one needs the AST half */
        mx_raise("cannot call '%s' on %s", name, mx_type_name(recv));
        return mx_null();   /* unreachable: mx_raise does not return */
    }
    /* a NATIVE first - the same order as the AST half and as the host's
     * eval_call. The emitted call for `os_sep()` is mx_call_list("os_sep",
     * null, ...), and the generated registry has no such entry, so without this
     * a compiled program would answer "Undefined function 'os_sep'" for a
     * builtin that the interpreted program runs happily. */
    {
        MxValue nv;
        if (mx_try_native(name, args, nargs, &nv)) return nv;
    }
    /* a FREE function. Three sources, in the host's order of preference: the
     * generated registry (the program's own functions), then GLOBALS for a
     * MODULE function (interp.mx calls parser.mx's parse_src by bare name, and
     * handle_import defines every module function in globals, interpreter.py:259),
     * then nothing - which is the host's "Undefined function" wording.
     *
     * The globals step is the mirror of the AST half's, added for the same
     * reason: a generated `parse_src(...)` and a walked `parse_src(...)` are the
     * same program text, and one of them resolving and the other not would be a
     * divergence that depends only on which half compiled the caller. */
    {
        MxFnGen fn = gen_fn_lookup(name);
        if (fn)
            return mx_call_fn_ptr(fn, p0, p1, p2, p3, p4, p5, nargs);
#ifdef MX_BOTH
        {
            MxValue gv;
            if (env_get(g_globals, name, &gv) && gv.tag == MX_FUNC) {
                MxValue margv[MX_MAX_PARAMS];
                margv[0] = p0; margv[1] = p1; margv[2] = p2;
                margv[3] = p3; margv[4] = p4; margv[5] = p5;
                return mx_call_fn_list((MxNode *)gv.as.box, margv, nargs,
                                       g_globals);
            }
        }
#endif
        mx_raise("Undefined function '%s'", name);
        return mx_null();   /* unreachable: mx_raise does not return */
    }
}
#endif /* MX_SUBSTRATE - the generated path needs MxClassDesc, which only it has */

/* g_globals is DEFINED with the generated runtime, above, under the same
 * MX_BOTH rule. This section keeps only the reset hook, because a generated
 * program and the AST half's own runner both want a "start from nothing" and
 * neither should reach into another section's variable to do it. */
#ifdef MX_SUBSTRATE
void mx_substrate_init(void) { g_globals = NULL; }
#endif



#if !defined(MX_SUBSTRATE) || defined(MX_BOTH)
/* The ONE globals. Declared (extern) above the generated runtime because the
 * generated path reads it too; DEFINED here because in a B-2 binary both halves
 * are present, and two globals would be two scopes. A B-2 binary is compiled
 * with MX_BOTH precisely so a module imported at RUN time - which is source -
 * can be walked by this half while the program itself runs generated. */
   /* the AST, lexer, parser and evaluator are
                       * replaced by generated code in B-2 */

/* ===================================================================== */
/* 5. The AST - COMPILE_DESIGN §2. Only the 12 kinds the corpus contains. */
/* ===================================================================== */

typedef enum {
    N_PROGRAM, N_LET, N_ASSIGN, N_PRINT, N_EXPRSTMT,
    N_THROW, N_TRYCATCH, N_RETURN, N_FUNCDEF,
    N_LITERAL, N_IDENT, N_BINOP, N_CALL, N_TYPEOF,
    N_ARRAYLIT, N_INDEX, N_UNARY,
    N_IF, N_WHILE, N_LOOP, N_FORIN, N_BREAK, N_CONTINUE,
    N_CLASSDEF, N_NEW, N_DOT, N_IMPORT, N_FIELD, N_SUPER
} NTag;

struct MxNode {
    NTag   tag;
    int    line;
    MxValue lit;            /* N_LITERAL */
    const char *name;       /* N_IDENT / N_LET / N_ASSIGN / N_FUNCDEF / N_CALL */
    const char *op;         /* N_BINOP */
    MxNode *a, *b;          /* operands / children */
    MxNode **args;          /* N_CALL's argument LIST (was a single `a`) */
    int      nargs;
    MxNode **body;          /* N_PROGRAM / N_FUNCDEF */
    int     nbody;
    MxNode *catch_body;     /* N_TRYCATCH */
    /* N_ARRAYLIT reuses body/nbody for its elements - one list type, not two. */
    const char *catch_var;  /* N_TRYCATCH */
    MxNode *else_body;      /* N_IF's else branch, or an else-IF (nested N_IF) */
    MxNode *callee;         /* N_CALL's callee NODE (an N_DOT for a method call) */
    /* N_FUNCDEF's parameter NAMES. A list rather than one string, because case
     * 19 needs `first_greater_than(xs, limit)` and interp.mx has 150 functions
     * with arbitrary arity. One `param` field would have meant a second
     * mechanism for the second parameter. */
    const char **params;
    int         nparams;
    /* --- the B-3.3 slots. Four new kinds needed four new shapes, and each is
     * named for what it holds rather than reused from an existing field: a
     * reader looking for a class's parent should find `parent`, not have to
     * know that `a` happens to hold it. */
    const char *parent;       /* N_CLASSDEF's parent name, or NULL */
    MxNode    **fields;       /* N_CLASSDEF's FIELD nodes */
    int         nfields;
    MxNode    **methods;      /* N_CLASSDEF's methods (NOT including init) */
    int         nmethods;
    MxNode     *init;         /* N_CLASSDEF's init, in its OWN slot - never in
                               * methods (AGENTS.md's invariant) */
    MxNode    **meth_bodies;  /* MxNode for each methods[] entry, parallel */
    int         nmeth_bodies;
    const char *field;        /* N_DOT's field name */
    const char *class_name;   /* N_NEW's class name, possibly dotted */
    /* mod_env - the __mod_env__ STAMP (COMPILE_DESIGN 8.1), as a void* to an
     * MxEnv. It is written into the AST by handle_import and read as the call
     * frame's PARENT at call time, so it is a RUNTIME property of a node - not
     * something a compiler may resolve at emit time, because whether a function
     * gets one depends on HOW it was imported. */
    void      *mod_env;
    void      *mod_env2;      /* the same stamp on a method or init node */
    /* var2 - N_FORIN's SECOND binding, NULL for the one-binding form. It is a
     * named field rather than a reuse of `parent` or `else_body` for the same
     * reason those are named: `parent` holding an index would be a reader's
     * trap. NULL and "" are DIFFERENT here - the evaluator branches on it. */
    const char *var2;
};

/* g_argv0 - the running binary's path, captured in main. The LAST import base is
 * this binary's directory (:201), and reading it at RUN time is what keeps D-1's
 * shape honest in C. */
static const char *g_argv0 = NULL;

/* mx_set_argv0 - the generated program's entry point calls this FIRST.
 *
 * The import walk's LAST base is the running binary's own directory (D-1's
 * shape: a compiled binary resolves relative to ITSELF, as main.py resolves
 * relative to interpreter.py). That base comes from argv[0], and in a B-2
 * (MX_BOTH) build there is no interpreter main to capture it - the generated
 * main() is the entry point. So the capture moves behind a call the emitted
 * prologue makes, and the AST half's own main makes the SAME call: one capture,
 * two entry points, rather than two captures that could disagree.
 *
 * Called with argv[0] == NULL it is a no-op, because a binary with no argv
 * (mx_substrate_init) has no directory to report and must not invent one. */
void mx_set_argv0(const char *argv0) {
    g_argv0 = argv0;
}

/* mx_set_argv_full - record argv itself, for sys_args.
 *
 * Separate from mx_set_argv0 because argv[0] is needed for the import walk's
 * last base while the REST is needed by sys_args, and the AST half already has
 * both in hand from its own main. One call each from two entry points, rather
 * than one function that takes both and forces the other half to synthesise the
 * piece it does not have. */
void mx_set_argv_full(char **argv, int argc) {
    mx_set_argv(argv, argc);
    if (argc > 0) mx_set_argv0(argv[0]);
}

/* mx_set_source - the SOURCE the binary was generated FROM.
 *
 * This is the second import base, and it is not argv[0]'s directory. The host's
 * rule is the ENTRY SCRIPT's directory (:200), and the compiled form of "the
 * entry script" is the .mx file this C was generated from - not the executable,
 * which lands wherever the build put it. Case 23 is the test: mod_helper.mx sits
 * beside 23_import_module.mx, and a binary built into gen/ would search gen/ and
 * report the module missing, with a 56-path not-found list naming directories
 * that have nothing to do with the program.
 *
 * So the emitter records the source path and passes it here, once, before any
 * statement runs. Passing it as a VALUE rather than compiling it in is what
 * keeps the binary relocatable: the same binary run from another directory still
 * resolves imports relative to the program it was made from, which is D-1's
 * shape and the reason the base is a runtime setter at all. */
/* g_script_dir is DEFINED with the import walk, below; only this setter needs
 * it from up here, and a declaration is cheaper than moving the variable away
 * from the walk that documents its meaning. */
extern const char *g_script_dir;

void mx_set_source(const char *src_path) {
    const char *slash;
    if (!src_path || !*src_path) return;
    g_current_file = src_path;
    slash = strrchr(src_path, '/');
    if (!slash) { g_script_dir = "."; return; }
    {
        size_t n = (size_t)(slash - src_path);
        char *d = (char *)arena_alloc(n + 1);
        memcpy(d, src_path, n);
        d[n] = '\0';
        g_script_dir = d;
    }
}

/* mx_bin_dir - the directory of the RUNNING BINARY, which is the last search
 * base. Found from MOUNTX_BIN_DIR when set, else from argv[0]. It is resolved at
 * RUN time and never baked in, so the compiled binary resolves imports relative
 * to itself - the shape D-1 names. */
static char g_bin_dir[1024];
static int  g_bin_dir_done = 0;
static const char *mx_bin_dir(void) {
    if (!g_bin_dir_done) {
        const char *e = getenv("MOUNTX_BIN_DIR");
        const char *a0 = g_argv0;
        char *slash;
        g_bin_dir_done = 1;
        if (e && *e) {
            snprintf(g_bin_dir, sizeof g_bin_dir, "%s", e);
        } else if (a0 && *a0) {
            snprintf(g_bin_dir, sizeof g_bin_dir, "%s", a0);
            slash = strrchr(g_bin_dir, '/');
            if (slash) *slash = '\0'; else snprintf(g_bin_dir, sizeof g_bin_dir, ".");
        } else {
            snprintf(g_bin_dir, sizeof g_bin_dir, ".");
        }
    }
    return g_bin_dir;
}

/* ---- the registry ITSELF: name -> class AST node ----------------------
 * A flat, growable array of (name, node) pairs rather than the instance
 * MxDict above, because a class node is an AST node and not a VALUE - putting
 * one in an MxValue would need a tag for it that no expression can produce.
 * The lookup cost is the price COMPILE_DESIGN 8.2 accepts knowingly. */
typedef struct { char *name; MxNode *node; } MxClassEntry;

/* g_receiver / g_receiver_class - the `this` and `super` of the CURRENT method.
 *
 * Two globals rather than two environment entries, because `this` is not a
 * binding a program can rebind (AGENTS.md: `this` resolves from the call frame)
 * and because `super` must reach the class that DEFINED the running method -
 * not the receiver's class - which is what makes a multi-level chain resolve
 * (interpreter.py:611-613).
 *
 * They are saved and restored around every call rather than cleared, so a method
 * that calls another method and then reads `super` still sees ITS OWN receiver.
 * That is the shape of a correct nesting, and getting it wrong is the difference
 * between working inheritance and a subclass quietly operating on its parent. */
static MxValue g_receiver;
static int     g_have_receiver = 0;
static MxNode *g_receiver_class = NULL;

/* chain_scratch - one buffer for class_chain, so the two errors that need a
 * chain can raise through the SAME function rather than a second walk. */
static MxNode *chain_scratch[64];

static MxClassEntry *g_classes = NULL;

/* The globals ENV, for resolve_class's dotted form (:443-448), which looks a
 * module up in globals by name. The AST-node registry above is separate and is
 * what `classx` registers into; this is the environment a module OBJECT lives
 * in, and the two must not be confused - a class name resolves through the
 * registry, a module name through the environment. */
/* The FOUR entry points the EMITTER calls. Public because generated code names
 * them: they are the substrate's CONTRACT with the emitter, not internals.
 * Declared here and defined below; mkhdr.py copies the real prototypes into
 * mini.h, so neither side keeps a second copy of a signature.
 *
 * Guarded with the descriptor they take: an interpreter build has no generated
 * classes, so MxClassDesc does not exist there and neither does the registry. */
#ifdef MX_SUBSTRATE
/* The native table's door, declared here because mx_call_list (above) asks it
 * and its definition is below with the other natives. One declaration, one
 * signature - the point of putting the sixteen rules in one function was to keep
 * them in one place, and a forward declaration is a smaller cost than splitting
 * them back out. */
MXFN void mx_register_class(MxClassDesc *d);
MXFN void mx_register_genfn(const char *name, MxFnGen fn);
MXFN MxFnGen gen_fn_lookup(const char *name);
MXFN MxValue mx_dot(MxValue obj, const char *field);
MXFN MxValue mx_new(const char *class_name, MxValue a0, int nargs);
MXFN MxValue mx_call_list(const char *name, MxValue recv,
                          MxValue p0, MxValue p1, MxValue p2, MxValue p3,
                          MxValue p4, MxValue p5, int nargs);
static MxValue mx_call_fn_list(MxNode *fn, MxValue *args, int nargs,
                               MxEnv *globals);
/* mx_new_from_module - `newx mod.Klass(...)` and `mod.Klass(...)`: a class that
 * lives in a MODULE. The class is an AST node in the shared registry (handle_import
 * defines module classes too), so this builds the instance the interpreter's way
 * rather than the generated way - and the generated way is not available, because
 * a module's class was never emitted. */
static MxValue mx_new_module_class(MxModule *mod, const char *name,
                                   MxValue *args, int nargs);

/* mx_new_module_class - `newx mod.Klass(args...)`.
 *
 * The host resolves the DOTTED class name in eval_new BEFORE it builds anything
 * (interpreter.py:639-645): it splits on the dot, looks the module up in
 * globals, and takes the class out of mod.classes. So this is that lookup, and
 * then the ordinary build - which lives in eval_new, not here. Duplicating the
 * three-step build would be a second copy of the merge rule; this calls the one
 * already written.
 *
 * Both error texts are the host's, including the capital M on the second: a
 * corpus golden compares bytes, and "Module 'x' has no class 'Y'" is a different
 * string from every other no-class message in the file. */
static MxValue mx_new_module_class(MxModule *mod, const char *name,
                                   MxValue *args, int nargs) {
    MxValue cv;
    MxNode *klass;
    if (!mx_dict_has(mod->classes, name))
        mx_raise("Module '%s' has no class '%s'", mod->mod_name, name);
    cv = mx_dict_get(mod->classes, name);
    klass = (MxNode *)cv.as.box;
    {
        /* the SAME build eval_new does: merged fields parent-first, then the
         * nearest init up the chain, with the arguments going to the init. */
        MxDict *fields = mx_dict_new(8);
        MxInstance *inst;
        MxNode *chain[MX_GEN_CHAIN_MAX];
        int nchain, i, j;
        nchain = mx_class_chain(klass, chain);
        /* MERGE parent-first, child-overriding, exactly as eval_new does
         * (interpreter.py:653-656): the chain reversed, and each field SET only
         * if a nearer class has not already claimed the name. A field's value is
         * an AST node, so it is EVALUATED - in the globals, because a class's
         * field initialiser is evaluated where the class is defined, and a
         * module class's own lets are already in scope by the time it is
         * registered. */
        for (i = nchain - 1; i >= 0; i--)
            for (j = 0; j < chain[i]->nfields; j++) {
                const char *fname = chain[i]->fields[j]->name;
                if (!mx_dict_has(fields, fname))
                    mx_dict_set(fields, fname,
                                mx_eval(chain[i]->fields[j]->a, g_globals));
            }
        inst = (MxInstance *)arena_alloc(sizeof(MxInstance));
        inst->stamp = mx_next_stamp();
        inst->klass = klass;
        inst->fields = fields;
        inst->class_name = klass->name;
        /* the NEAREST init up the chain, run with the receiver bound and the
         * arguments passed - eval_new's step three. Stopping at the first init
         * found from the class itself UPWARD is what makes an inherited init
         * apply when the subclass defines none. */
        for (i = 0; i < nchain; i++)
            if (chain[i]->init)
                return mx_call_instance(chain[i]->init, args, nargs, inst, chain[i]);
        {
            MxValue r;
            r.tag = MX_INST; r.as.box = inst;
            return r;
        }
    }
}

#endif /* MX_SUBSTRATE - the generated path's entry points */

/* g_globals_env and mx_set_globals_env are GONE. They were a second name for
 * g_globals, which now exists in both builds (the generated runtime declares it,
 * the AST half defines it), so the duplicate was a second variable that the AST
 * half's main set and nothing else read - and under MX_BOTH it compiled to a
 * -Wunused-function warning, because the generated main sets g_globals directly
 * and never calls the setter. One globals, one name, one place it is set. */

/* mx_eval - the evaluator, defined in section 8. Forward-declared here because
 * eval_new (section 5) calls it to evaluate a field's initialiser and a
 * constructor argument. */
static MxValue mx_eval(MxNode *n, MxEnv *env);
/* mx_call_instance is defined with the rest of the calling convention; eval_new
 * (section 5) needs it to run a constructor, and class_chain needs MxValue
 * returned from it. Forward declarations, rather than moving either, because the
 * dependency is real and reordering the sections would hide it. */
static MxValue mx_call_instance(MxNode *fn, MxValue *argv, int nargs,
                                MxInstance *inst, MxNode *defining);
static size_t         g_nclasses = 0, g_capclasses = 0;

MXFN void mx_define_class(const char *name, MxNode *node) {
    size_t i;
    /* define, not assign-in-place: `handle_import` (:250) registers a module's
     * classes too, and a LATER import of the same name replaces the earlier one
     * while keeping the registry's growth order. */
    for (i = 0; i < g_nclasses; i++) {
        if (strcmp(g_classes[i].name, name) == 0) {
            g_classes[i].node = node;
            return;
        }
    }
    if (g_nclasses >= g_capclasses) {
        size_t ncap = g_capclasses ? g_capclasses * 2 : 8;
        MxClassEntry *ne = (MxClassEntry *)arena_alloc(ncap * sizeof(MxClassEntry));
        for (i = 0; i < g_nclasses; i++) ne[i] = g_classes[i];
        g_classes = ne;
        g_capclasses = ncap;
    }
    g_classes[g_nclasses].name = arena_strdup(name, strlen(name));
    g_classes[g_nclasses].node = node;
    g_nclasses++;
}

/* mx_resolve_class - NAME -> AST node, or NULL. Plain names check the global
 * registry FIRST (:439), which is what makes a local class shadow a module
 * class of the same name; the dotted form ("dict.Dict") is only tried when the
 * plain name missed (:441-448). The order is the specification, not a detail -
 * reversing it makes a module class win over a local one. */
static MxNode *mx_resolve_class(const char *name) {
    if (!name) return NULL;
    const char *iname = mx_fast_intern(name);
    size_t i;
    for (i = 0; i < g_nclasses; i++) {
        if (g_classes[i].name == iname) return g_classes[i].node;
    }
    return NULL;
}

/* mx_find_method - a method in ONE class. `init` lives in its OWN slot, not in
 * the methods list (AGENTS.md: "init is NOT in methods"), so it is checked
 * explicitly and FIRST. This split is the invariant whose absence broke a
 * release once ("super has no method 'init'"), and it is why find_method is the
 * ONLY lookup path: a second one that iterated `methods` would silently miss
 * init, and `newx` on an inherited-init class would run no constructor. */
static MxNode *mx_find_method(MxNode *klass, const char *name) {
    int i;
    if (!klass || !name) return NULL;
    if (strcmp(name, "init") == 0 && klass->init != NULL) return klass->init;
    for (i = 0; i < klass->nmethods; i++) {
        const char *mn = mx_class_name_of(klass->methods[i]);
        if (mn == name || strcmp(mn, name) == 0) return klass->methods[i];
    }
    return NULL;
}

/* mx_class_chain - [klass, parent, grandparent, ...], with the two validation
 * errors raised on the way (interpreter.py:451-468). It returns the LENGTH and
 * fills out[], so the caller's buffer is the chain - there is no allocation and
 * no ownership question.
 *
 * Cycle detection is a VISITED POINTER ARRAY, which is the C equivalent of the
 * host's `seen` set of id()s: the host compares id(cur), and a pointer IS the
 * identity here. It must be a visited set rather than a depth limit, because a
 * DIAMOND (two parents sharing one grandparent) is legitimate and would be
 * misread as a cycle by a counter. */
#define MX_CHAIN_MAX 64
#ifdef MX_BOTH
_Static_assert(MX_CHAIN_MAX == MX_GEN_CHAIN_MAX,
               "the two class-chain bounds must be the same number");
#endif
static int mx_class_chain(MxNode *klass, MxNode **out) {
    int n = 0;
    MxNode *seen[MX_CHAIN_MAX];
    MxNode *cur = klass;
    while (cur != NULL) {
        int i, revisited = 0;
        for (i = 0; i < n; i++) if (seen[i] == cur) { revisited = 1; break; }
        if (revisited) {
            mx_raise("Inheritance cycle involving class '%s'", mx_class_name_of(cur));
            return -1;
        }
        if (n >= MX_CHAIN_MAX) {
            /* Refuse rather than truncate: a chain this deep is a program bug,
             * and a silent truncation would make a cycle past the limit look
             * like a legitimate end of chain. */
            mx_raise("Inheritance chain deeper than %d classes", MX_CHAIN_MAX);
            return -1;
        }
        seen[n] = cur;
        out[n++] = cur;
        if (cur->parent == NULL || cur->parent[0] == '\0') break;
        cur = mx_resolve_class(cur->parent);
        if (cur == NULL) {
            mx_raise("Class '%s' extends unknown class '%s'", mx_class_name_of(out[n - 1]),
                     cur ? "" : out[n - 1]->parent);
            return -1;
        }
    }
    return n;
}

/* env_get_or_null - like env_get but a MISSING name yields null instead of
 * raising. resolve_class's dotted form wraps the lookup in a try/except
 * (interpreter.py:442-444), so "the module name is not bound" is a normal
 * answer there, not an error - and a raising lookup would turn it into one. */


static MxValue env_get_or_null(MxEnv *e, const char *name) {
    MxValue v;
    if (e && env_get(e, name, &v)) return v;
    return mx_null();
}

#if !defined(MX_SUBSTRATE) || defined(MX_BOTH)
/* Only the INTERPRETER half has AST nodes, so only it needs a node's class
 * name. A substrate build reports it "used but never defined" because the
 * definition is compiled out - which is correct: there are no nodes to name. */

static const char *mx_class_name_of(void *klass_node) {
    return klass_node ? ((MxNode *)klass_node)->name : "?";
}
#endif
#ifdef MX_SUBSTRATE
/* In a BOTH build these were ALREADY defined in the substrate arm above, so
 * they are guarded rather than repeated: two definitions is a link error, and
 * the pair is only ambiguous in the build that has both halves - which is
 * exactly the build a B-2 module import needs. A generated instance is never
 * created in such a build, so the substrate arm's reading is the right one. */
#else
MXFN const char *mx_inst_class_name(MxValue v) {
    return mx_class_name_of(((MxInstance *)v.as.box)->klass);
}
MXFN MxDict *mx_inst_fields(MxValue v) {
    return ((MxInstance *)v.as.box)->fields;
}
#endif

/* mx_import_module - the emitter's entry point for `importx name`.
 *
 * It is a THIN wrapper, not a reimplementation: mx_exec_import already holds the
 * four-base walk, the dedup rule and the not-found text, and mx_load_module
 * already builds the module box with its three export slots. So this resolves the
 * name (or imports it) and hands back the box.
 *
 * It RETURNS the box rather than binding it, so the emitted env_define is the one
 * place a name is bound - and an import of a module that is already imported
 * returns the EXISTING box, which is what makes a second importx the same object
 * (module objects are never shadowed, AGENTS.md).
 *
 * The one thing it has to do itself is BUILD the import node, because
 * mx_exec_import takes an MxNode. That is why a B-2 binary is compiled with
 * MX_BOTH: a module imported at RUN time is source, so it needs the AST half to
 * read it. Nothing is compiled at run time - the module is walked, not emitted -
 * which is the honest scope of B-3.3 and the reason the module's functions are
 * MxFuncGen values looked up by name through the module's funcs slot. */
static void mx_exec_import(MxNode *s, MxEnv *env);
/* Deliberately NOT static. Generated code calls it by name, and in an AST-only
 * build nothing does - which is correct, because an AST-only build runs corpus
 * files through the AST half's own mx_exec_import. A static would be an
 * unused-function warning in exactly the build that has no caller, so the
 * export is the honest shape: it is part of the substrate's contract. */
MxValue mx_import_module(const char *name) {
    MxValue existing;
    if (env_get(g_globals, name, &existing) && existing.tag == MX_MOD) return existing;
    {
        MxNode *st = (MxNode *)arena_alloc(sizeof(MxNode));
        st->tag = N_IMPORT;
        st->name = (char *)name;
        mx_exec_import(st, g_globals);
    }
    if (env_get(g_globals, name, &existing) && existing.tag == MX_MOD) return existing;
    mx_raise("Module '%s' not found", name);
    return mx_null();
}


/* ---- the 16 missing NATIVES (B-3.4, the crown run) --------------------
 *
 * ONE function, `mx_try_native`, matched BEFORE the user-function lookup by
 * BOTH halves: the AST half's N_CALL arm and the generated path's
 * mx_call_list. One implementation rather than two, because AGENTS.md already
 * says the authoritative registry is the code itself, and because sixteen
 * natives transcribed twice is thirty-two chances to drift.
 *
 * Every message is the HOST's, verbatim - `f"{name} expects 0 arguments"`,
 * `idiv: division by zero`, and so on. A golden's error text is compared byte
 * for byte, so a paraphrase is a divergence even when it is a better sentence.
 *
 * The arity and type GUARDS come before the work, in the host's order, and
 * `is_num` must exclude bool (AGENTS.md) - `sys_exit(true)` is not exit 1.
 *
 * Returns 1 if it HANDLED the call (and stores the value in *out), else 0 so
 * the caller falls through to the user-function lookup. A native that returns
 * null legitimately is still "handled" - that is why the answer is an int, not
 * the value: `sleep_ms` and `sys_exit` return nothing at all. */

/* g_argv / g_argc - the process arguments, for sys_args.
 *
 * SET ONCE, by mx_set_argv, and read by sys_args. It is argv+1 rather than the
 * host's argv+2 because the host's argv is [python3, main.py, prog.mx, ...]:
 * two entries name the harness and the program. Here the program IS argv[0], so
 * the equivalent - the arguments after the program, which is what a guest wants
 * - is argv+1. The I-M7 note about the "one step aside" is about the LAYER
 * boundary; this is the same rule stated for a compiled entry point. */
static char **g_argv = NULL;
static int    g_argc = 0;

void mx_set_argv(char **argv, int argc) {
    g_argv = argv;
    g_argc = argc;
}

/* mx_num_of - a numeric value as a double.
 *
 * The value carries EITHER an int64 (MX_INT) or a double (MX_NUM) in the SAME
 * union member pair, so reading as.d unconditionally reads the bits of an
 * integer as a double. That is not a rounding error - it is a garbage number,
 * and the first version of idiv returned -9223372036854775808 for `idiv(-7, 2)`.
 *
 * The tag is what says which member is live. AGENTS.md's rule that is_num must
 * exclude bool is the same fact seen from the other side: a bool is neither, so
 * it is refused by the guard before this is ever reached. */
static double mx_num_of(MxValue v) {
    return v.tag == MX_INT ? (double)v.as.i : v.as.d;
}

/* mx_idiv - integer division truncating TOWARD ZERO (interpreter.py:749-757).
 *
 * The host computes abs//abs and then applies the sign, so `7/2 -> 3` and
 * `-7/2 -> -3`. Note the host's `//` FLOORS, so the magnitude step must floor
 * too: plain C division already truncates, but the two agree only because the
 * operands are made positive first. Writing it the host's way keeps the negative
 * case obviously right to a reader, and a silently wrong quotient for negative
 * operands is the kind of defect no differential on positive numbers shows.
 *
 * An INTEGER pair stays exact: two int64s are divided as int64s, so
 * `idiv(2**62, 1)` does not lose the low bit through a double. */
static MxValue mx_idiv(MxValue a, MxValue b) {
    int neg = ((mx_num_of(a) < 0) != (mx_num_of(b) < 0));
    if (a.tag == MX_INT && b.tag == MX_INT) {
        int64_t x = a.as.i, y = b.as.i, q;
        uint64_t ux = x < 0 ? (uint64_t)(-x) : (uint64_t)x;
        uint64_t uy = y < 0 ? (uint64_t)(-y) : (uint64_t)y;
        q = (int64_t)(ux / uy);
        return mx_int(neg ? -q : q);
    }
    {
        double x = mx_num_of(a), y = mx_num_of(b);
        double m = floor((x < 0 ? -x : x) / (y < 0 ? -y : y));
        /* The RESULT is a FLOAT whenever an operand is, because Python's `//`
         * on two floats is a float: `7.0 // 2` is 3.0, not 3. Returning an int
         * here printed "3" where the host prints "3.0", which is a one-character
         * difference in six places and a wrong answer in the sense that
         * matters - `idiv(a, 2) / 2` would then divide an integer. The int/int
         * case above is the only one that stays exact. */
        if (a.tag == MX_NUM || b.tag == MX_NUM)
            return mx_num(neg ? -m : m);
        return mx_int(neg ? -(int64_t)m : (int64_t)m);
    }
}

/* ---- the db_* natives, against libsqlite3 (B-3.5) ----------------------
 *
 * CB-E originally said "no dependencies beyond libc" and these four REFUSED,
 * loudly, because this box had no libsqlite3. The library is present now, so
 * they work - and the refusal text is gone with it rather than kept as a
 * fallback: a native that prefers the refusal over the real answer would be a
 * worse neighbour than one that simply does its job.
 *
 * Every shape here is the HOST's (interpreter.py:755-827), and two of them are
 * load-bearing in a way that is easy to miss:
 *
 *   - db_exec returns sqlite's OWN rowcount, so a DDL statement reports -1.
 *     That is SQLite semantics leaking through the host's return value, and
 *     fbtest asserts it ("DDL exec -> -1 rowcount"). Computing "rows changed"
 *     ourselves would answer 0 and fail a check the host passes.
 *   - db_query returns an array of arrays, each row a fresh array - not a row
 *     object, and not a shared one.
 *
 * Bind parameters are bound, NEVER concatenated. AGENTS.md says it outright
 * ("Never build SQL by concatenation - use the _params variants") and it is the
 * only reason a caller can pass data it did not write. */
#include <sqlite3.h>

#define MX_DB_MAX_CONN 32
/* g_dbconn is indexed by CONNECTION ID, and an id starts at 1 so that 0 can
 * mean "no connection" - which is what a default-initialised slot is. There is
 * no counter: the id IS the index, and the free slot is found by scanning. A
 * separate counter would be a second thing to keep consistent with the table. */
static sqlite3   *g_dbconn[MX_DB_MAX_CONN];

static sqlite3 *db_conn_of(MxValue v, const char *who) {
    int64_t cid = v.as.i;
    if (cid < 1 || cid >= MX_DB_MAX_CONN || !g_dbconn[cid]) {
        mx_raise("%s: unknown connection %lld", who, (long long)cid);
        return NULL;
    }
    return g_dbconn[cid];
}

/* bind_params - bind an optional third argument to `?` placeholders.
 *
 * Returns the number bound, or -1 when there was no params argument at all
 * (which the host models as None and which therefore means "execute the SQL
 * as written"). A params value that is not an array is the host's own error. */
static int db_bind(sqlite3_stmt *st, MxValue *a, int n, int start, const char *who) {
    int i;
    if (n <= start) return -1;
    if (a[start].tag != MX_ARR)
        mx_raise("%s params must be an array", who);
    for (i = 0; i < (int)a[start].as.arr->n; i++) {
        MxValue v = a[start].as.arr->v[i];
        int rc;
        switch (v.tag) {
            case MX_INT:  rc = sqlite3_bind_int64(st, i + 1, v.as.i); break;
            case MX_NUM:  rc = sqlite3_bind_double(st, i + 1, v.as.d); break;
            case MX_STR:  rc = sqlite3_bind_text(st, i + 1, v.as.s, -1, SQLITE_TRANSIENT); break;
            /* SQLite has no boolean and no null of ours: NULL is bound for
             * nullx and TRUE/FALSE as 1/0, which is what Python's sqlite3 does
             * with them and therefore what the host does. */
            case MX_BOOL: rc = sqlite3_bind_int(st, i + 1, v.as.b ? 1 : 0); break;
            default:      rc = sqlite3_bind_null(st, i + 1); break;
        }
        if (rc != SQLITE_OK) mx_raise("bind failed");
    }
    return (int)a[start].as.arr->n;
}


/* The four natives. Each keeps the host's arity/type guard FIRST, so a misuse
 * reports the host's wording rather than whatever sqlite would have said. */
static MxValue db_open_n(MxValue *a, int n) {
    sqlite3 *db = NULL;
    int cid;

    if (n != 1 || a[0].tag != MX_STR) mx_raise("db_open expects 1 string path");
    for (cid = 1; cid < MX_DB_MAX_CONN; cid++) if (!g_dbconn[cid]) break;
    if (cid >= MX_DB_MAX_CONN) mx_raise("db_open: too many open connections");
    if (sqlite3_open(a[0].as.s, &db) != SQLITE_OK) {
        const char *msg = db ? sqlite3_errmsg(db) : "cannot open";
        mx_raise("db_open failed: %s", msg);
    }
    g_dbconn[cid] = db;
    return mx_int(cid);
}

static MxValue db_close_n(MxValue *a, int n) {
    int64_t cid;
    if (n != 1 || !mx_is_num(a[0])) mx_raise("db_close expects 1 connection id");
    cid = a[0].as.i;
    if (cid < 1 || cid >= MX_DB_MAX_CONN || !g_dbconn[cid])
        mx_raise("db_close: unknown connection %lld", (long long)cid);
    sqlite3_close(g_dbconn[cid]);
    g_dbconn[cid] = NULL;
    return mx_null();
}

static MxValue db_exec_n(MxValue *a, int n) {
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    MxValue out;
    int nb;
    if (n < 2 || !mx_is_num(a[0]) || a[1].tag != MX_STR)
        mx_raise("db_exec expects (conn_id, sql, [params])");
    db = db_conn_of(a[0], "db_exec");
    if (sqlite3_prepare_v2(db, a[1].as.s, -1, &st, NULL) != SQLITE_OK)
        mx_raise("db_exec failed: %s", sqlite3_errmsg(db));
    nb = db_bind(st, a, n, 2, "db_exec");
    (void)nb;
    if (sqlite3_step(st) != SQLITE_DONE) {
        const char *msg = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        mx_raise("db_exec failed: %s", msg);
    }
    sqlite3_finalize(st);
    /* The ROWCOUNT, with Python's semantics rather than SQLite's. The host
     * returns cur.rowcount, which Python sets only for INSERT/UPDATE/DELETE/
     * REPLACE and leaves at -1 for everything else - so a CREATE TABLE reports
     * -1, and fbtest asserts exactly that ("DDL exec -> -1 rowcount").
     *
     * sqlite3_changes() alone cannot express it: it reports the most recent
     * DML's count regardless of what just ran, so after a CREATE it answers 0
     * and the check fails on a statement the host passes. So the leading keyword
     * decides, and the fallback is -1. */
    {
        const char *sql = a[1].as.s;
        while (*sql == ' ' || *sql == '\n' || *sql == '\t' || *sql == '\r') sql++;
        if (strncmp(sql, "INSERT", 6) == 0 || strncmp(sql, "UPDATE", 6) == 0 ||
            strncmp(sql, "DELETE", 6) == 0 || strncmp(sql, "REPLACE", 7) == 0)
            out = mx_int(sqlite3_changes(db));
        else
            out = mx_int(-1);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return out;
}

static MxValue db_query_n(MxValue *a, int n) {
    sqlite3 *db;
    sqlite3_stmt *st = NULL;
    MxArray *rows;
    MxValue out;
    int nb;
    if (n < 2 || !mx_is_num(a[0]) || a[1].tag != MX_STR)
        mx_raise("db_query expects (conn_id, sql, [params])");
    db = db_conn_of(a[0], "db_query");
    if (sqlite3_prepare_v2(db, a[1].as.s, -1, &st, NULL) != SQLITE_OK)
        mx_raise("db_query failed: %s", sqlite3_errmsg(db));
    nb = db_bind(st, a, n, 2, "db_query");
    (void)nb;
    /* mx_arr_new(n) builds an array of n ALREADY-PRESENT slots - it is the
     * array-literal constructor, so `n` is the length, not a capacity. A query
     * needs an EMPTY array with room to grow, so the length is zeroed right
     * after. Leaving it at 8 returned eight nulls in front of every result. */
    rows = mx_arr_new(8);
    rows->n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int ncol = sqlite3_column_count(st);
        MxArray *row = mx_arr_new((size_t)ncol);
        int c;
        for (c = 0; c < ncol; c++) {
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER:
                    row->v[c] = mx_int(sqlite3_column_int64(st, c)); break;
                case SQLITE_FLOAT:
                    row->v[c] = mx_num(sqlite3_column_double(st, c)); break;
                case SQLITE_NULL:
                    row->v[c] = mx_null(); break;
                default: {
                    /* COPY the bytes. sqlite3_column_text returns a pointer the
                     * library owns and INVALIDATES at the next step/reset, and
                     * mx_str stores the pointer rather than the characters - so
                     * storing it directly returned the last row's bytes for every
                     * row after it. Copying into the arena costs one copy and
                     * turns a data-dependent garbage read into a value. */
                    const unsigned char *txt = sqlite3_column_text(st, c);
                    const char *str = txt ? (const char *)txt : "";
                    size_t n = strlen(str);
                    char *copy = (char *)arena_alloc(n + 1);
                    memcpy(copy, str, n + 1);
                    row->v[c] = mx_str(copy);
                    break;
                }
            }
        }
        if (rows->n >= rows->cap) {
            /* grow, so a query returning more rows than the initial capacity
             * appends rather than overrunning - the same reason mx_arr_value
             * never hands out a fixed buffer. */
            size_t cap = rows->cap * 2;
            MxValue *nv = (MxValue *)arena_alloc(cap * sizeof(MxValue));
            memcpy(nv, rows->v, rows->n * sizeof(MxValue));
            rows->v = nv; rows->cap = cap;
        }
        rows->v[rows->n++] = mx_arr_value(row);
    }
    sqlite3_finalize(st);
    out = mx_arr_value(rows);
    return out;
}

/* --- HTTP client routines (via curl) ------------------------------------- */

#ifdef _WIN32
static char *mx_escape_shell_arg(const char *s) {
    if (!s) {
        char *o = (char *)malloc(3);
        if (o) strcpy(o, "\"\"");
        return o;
    }
    size_t len = strlen(s);
    size_t cap = len * 2 + 4;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') {
            out[j++] = '\\';
            out[j++] = '"';
        } else if (s[i] == '%') {
            out[j++] = '%';
            out[j++] = '%';
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}
#else
static char *mx_escape_shell_arg(const char *s) {
    if (!s) {
        char *o = (char *)malloc(3);
        if (o) strcpy(o, "''");
        return o;
    }
    size_t len = strlen(s);
    size_t cap = len * 4 + 4;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}
#endif

static MxValue mx_http_get_impl(const char *url) {
    char *esc_url = mx_escape_shell_arg(url);
    if (!esc_url) mx_raise("http_get out of memory");
    size_t cmd_len = strlen(esc_url) + 128;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_url); mx_raise("http_get out of memory"); }
    snprintf(cmd, cmd_len, "curl -s -S -f -L --max-time 15 %s 2>&1", esc_url);
    free(esc_url);

    FILE *fp;
#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    free(cmd);
    if (!fp) mx_raise("http_get failed: unable to run curl");

    size_t cap = 2048, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
#ifdef _WIN32
        _pclose(fp);
#else
        pclose(fp);
#endif
        mx_raise("http_get out of memory");
    }
    char chunk[1024];
    size_t nread;
    while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + nread + 1 > cap) {
            cap = (cap * 2 > len + nread + 1) ? cap * 2 : len + nread + 2048;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
#ifdef _WIN32
                _pclose(fp);
#else
                pclose(fp);
#endif
                mx_raise("http_get out of memory");
            }
            buf = nb;
        }
        memcpy(buf + len, chunk, nread);
        len += nread;
    }
    buf[len] = '\0';

#ifdef _WIN32
    int status = _pclose(fp);
    int code = status;
#else
    int status = pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif

    if (code != 0) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
            buf[--len] = '\0';
        }
        char msg[512];
        snprintf(msg, sizeof(msg), "http_get failed: %s", len > 0 ? buf : "request error");
        free(buf);
        mx_raise("%s", msg);
    }

    MxValue result = mx_str(arena_strdup(buf, len));
    free(buf);
    return result;
}

static MxValue mx_http_post_impl(const char *url, const char *body, size_t body_len, const char *content_type) {
    char tmp_path[256];
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    if (GetTempFileNameA(tmp_dir, "mxp", 0, tmp_path) == 0) {
        mx_raise("http_post failed: unable to create temporary file");
    }
    FILE *tf = fopen(tmp_path, "wb");
    if (!tf) mx_raise("http_post failed: unable to write temporary file");
    if (body_len > 0) fwrite(body, 1, body_len, tf);
    fclose(tf);
#else
    strcpy(tmp_path, "/tmp/mx_post_XXXXXX");
    int fd = mkstemp(tmp_path);
    if (fd < 0) mx_raise("http_post failed: unable to create temporary file");
    if (body_len > 0) {
        ssize_t w = write(fd, body, body_len);
        (void)w;
    }
    close(fd);
#endif

    char *esc_url = mx_escape_shell_arg(url);
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "Content-Type: %s", content_type ? content_type : "application/x-www-form-urlencoded");
    char *esc_hdr = mx_escape_shell_arg(hdr);

    if (!esc_url || !esc_hdr) {
#ifdef _WIN32
        DeleteFileA(tmp_path);
#else
        unlink(tmp_path);
#endif
        if (esc_url) free(esc_url);
        if (esc_hdr) free(esc_hdr);
        mx_raise("http_post out of memory");
    }

    size_t cmd_len = strlen(esc_url) + strlen(esc_hdr) + strlen(tmp_path) + 160;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) {
#ifdef _WIN32
        DeleteFileA(tmp_path);
#else
        unlink(tmp_path);
#endif
        free(esc_url);
        free(esc_hdr);
        mx_raise("http_post out of memory");
    }

#ifdef _WIN32
    snprintf(cmd, cmd_len, "curl -s -S -f -L --max-time 15 -X POST -H %s --data-binary \"@%s\" %s 2>&1",
             esc_hdr, tmp_path, esc_url);
#else
    snprintf(cmd, cmd_len, "curl -s -S -f -L --max-time 15 -X POST -H %s --data-binary '@%s' %s 2>&1",
             esc_hdr, tmp_path, esc_url);
#endif
    free(esc_url);
    free(esc_hdr);

    FILE *fp;
#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    free(cmd);
    if (!fp) {
#ifdef _WIN32
        DeleteFileA(tmp_path);
#else
        unlink(tmp_path);
#endif
        mx_raise("http_post failed: unable to run curl");
    }

    size_t cap = 2048, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
#ifdef _WIN32
        _pclose(fp);
        DeleteFileA(tmp_path);
#else
        pclose(fp);
        unlink(tmp_path);
#endif
        mx_raise("http_post out of memory");
    }

    char chunk[1024];
    size_t nread;
    while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + nread + 1 > cap) {
            cap = (cap * 2 > len + nread + 1) ? cap * 2 : len + nread + 2048;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
#ifdef _WIN32
                _pclose(fp);
                DeleteFileA(tmp_path);
#else
                pclose(fp);
                unlink(tmp_path);
#endif
                mx_raise("http_post out of memory");
            }
            buf = nb;
        }
        memcpy(buf + len, chunk, nread);
        len += nread;
    }
    buf[len] = '\0';

#ifdef _WIN32
    int status = _pclose(fp);
    DeleteFileA(tmp_path);
    int code = status;
#else
    int status = pclose(fp);
    unlink(tmp_path);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif

    if (code != 0) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
            buf[--len] = '\0';
        }
        char msg[512];
        snprintf(msg, sizeof(msg), "http_post failed: %s", len > 0 ? buf : "request error");
        free(buf);
        mx_raise("%s", msg);
    }

    MxValue result = mx_str(arena_strdup(buf, len));
    free(buf);
    return result;
}

/* =========================================================================
 * TCP Sockets Subsystem
 * ========================================================================= */
#define MX_MAX_SOCKETS 128
#ifdef _WIN32
static SOCKET g_sockets[MX_MAX_SOCKETS];
static int g_wsa_init = 0;
static void mx_ensure_wsa(void) {
    if (!g_wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        g_wsa_init = 1;
    }
}
static int mx_alloc_sock_slot(SOCKET s) {
    for (int i = 1; i < MX_MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = s;
            return i;
        }
    }
    return -1;
}
#else
static int g_sockets[MX_MAX_SOCKETS];
static int mx_alloc_sock_slot(int s) {
    for (int i = 1; i < MX_MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = s;
            return i;
        }
    }
    return -1;
}
#endif

static MxValue mx_tcp_listen_impl(MxValue *a, int n) {
    if (n < 1 || n > 2 || !mx_is_num(a[0]) || (n == 2 && a[1].tag != MX_STR))
        mx_raise("tcp_listen expects (port, [host])");
    int port = (int)a[0].as.i;
    const char *host = (n == 2) ? a[1].as.s : "0.0.0.0";
#ifdef _WIN32
    mx_ensure_wsa();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) mx_raise("tcp_listen failed to create socket");
    char opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        closesocket(s);
        mx_raise("tcp_listen bind failed on port %d", port);
    }
    if (listen(s, 128) != 0) {
        closesocket(s);
        mx_raise("tcp_listen listen failed");
    }
    int slot = mx_alloc_sock_slot(s);
    if (slot < 0) {
        closesocket(s);
        mx_raise("tcp_listen: too many open sockets");
    }
    return mx_int(slot);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) mx_raise("tcp_listen failed to create socket: %s", strerror(errno));
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(s);
        mx_raise("tcp_listen bind failed on port %d: %s", port, strerror(errno));
    }
    if (listen(s, 128) < 0) {
        close(s);
        mx_raise("tcp_listen listen failed: %s", strerror(errno));
    }
    int slot = mx_alloc_sock_slot(s);
    if (slot < 0) {
        close(s);
        mx_raise("tcp_listen: too many open sockets");
    }
    return mx_int(slot);
#endif
}

static MxValue mx_tcp_accept_impl(MxValue *a, int n) {
    if (n != 1 || !mx_is_num(a[0]))
        mx_raise("tcp_accept expects 1 socket descriptor");
    int slot = (int)a[0].as.i;
    if (slot <= 0 || slot >= MX_MAX_SOCKETS || !g_sockets[slot])
        mx_raise("tcp_accept: unknown or closed socket %d", slot);
#ifdef _WIN32
    struct sockaddr_in caddr;
    int clen = sizeof(caddr);
    SOCKET cs = accept(g_sockets[slot], (struct sockaddr *)&caddr, &clen);
    if (cs == INVALID_SOCKET) mx_raise("tcp_accept failed");
    int cslot = mx_alloc_sock_slot(cs);
    if (cslot < 0) {
        closesocket(cs);
        mx_raise("tcp_accept: too many open sockets");
    }
    char *ip = inet_ntoa(caddr.sin_addr);
    MxArray *res = mx_arr_new(2);
    res->v[0] = mx_int(cslot);
    res->v[1] = mx_str(arena_strdup(ip, strlen(ip)));
    res->n = 2;
    return mx_arr_value(res);
#else
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int cs;
    do {
        cs = accept(g_sockets[slot], (struct sockaddr *)&caddr, &clen);
    } while (cs < 0 && (errno == EINTR || errno == ECONNABORTED));
    if (cs < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return mx_null();
        mx_raise("tcp_accept failed: %s", strerror(errno));
    }
    int cslot = mx_alloc_sock_slot(cs);
    if (cslot < 0) {
        close(cs);
        mx_raise("tcp_accept: too many open sockets");
    }
    char *ip = inet_ntoa(caddr.sin_addr);
    MxArray *res = mx_arr_new(2);
    res->v[0] = mx_int(cslot);
    res->v[1] = mx_str(arena_strdup(ip, strlen(ip)));
    res->n = 2;
    return mx_arr_value(res);
#endif
}

static MxValue mx_tcp_recv_impl(MxValue *a, int n) {
    if (n != 2 || !mx_is_num(a[0]) || !mx_is_num(a[1]))
        mx_raise("tcp_recv expects (fd, max_len)");
    int slot = (int)a[0].as.i;
    int64_t max_len = a[1].as.i;
    if (slot <= 0 || slot >= MX_MAX_SOCKETS || !g_sockets[slot])
        mx_raise("tcp_recv: unknown or closed socket %d", slot);
    if (max_len <= 0) max_len = 4096;
    if (max_len > 1048576) max_len = 1048576;
    char *buf = (char *)malloc(max_len + 1);
    if (!buf) mx_raise("tcp_recv out of memory");
#ifdef _WIN32
    int r = recv(g_sockets[slot], buf, (int)max_len, 0);
#else
    ssize_t r = recv(g_sockets[slot], buf, (size_t)max_len, 0);
#endif
    if (r <= 0) {
        free(buf);
        return mx_str("");
    }
    buf[r] = '\0';
    MxValue out = mx_str(arena_strdup(buf, r));
    free(buf);
    return out;
}

static MxValue mx_tcp_send_impl(MxValue *a, int n) {
    if (n != 2 || !mx_is_num(a[0]) || a[1].tag != MX_STR)
        mx_raise("tcp_send expects (fd, string_data)");
    int slot = (int)a[0].as.i;
    if (slot <= 0 || slot >= MX_MAX_SOCKETS || !g_sockets[slot])
        mx_raise("tcp_send: unknown or closed socket %d", slot);
    const char *s = a[1].as.s;
    size_t len = strlen(s);
#ifdef _WIN32
    int sent = send(g_sockets[slot], s, (int)len, 0);
    if (sent < 0) mx_raise("tcp_send failed");
#else
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
    signal(SIGPIPE, SIG_IGN);
    ssize_t sent = send(g_sockets[slot], s, len, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EPIPE || errno == ECONNRESET) return mx_int(0);
        mx_raise("tcp_send failed: %s", strerror(errno));
    }
#endif
    return mx_int(sent);
}

static MxValue mx_tcp_close_impl(MxValue *a, int n) {
    if (n != 1 || !mx_is_num(a[0]))
        mx_raise("tcp_close expects 1 socket descriptor");
    int slot = (int)a[0].as.i;
    if (slot > 0 && slot < MX_MAX_SOCKETS && g_sockets[slot]) {
#ifdef _WIN32
        closesocket(g_sockets[slot]);
#else
        close(g_sockets[slot]);
#endif
        g_sockets[slot] = 0;
    }
    return mx_null();
}

static MxValue mx_tcp_connect_impl(MxValue *a, int n) {
    if (n != 2 || a[0].tag != MX_STR || !mx_is_num(a[1]))
        mx_raise("tcp_connect expects (host, port)");
    const char *host = a[0].as.s;
    int port = (int)a[1].as.i;
#ifdef _WIN32
    mx_ensure_wsa();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) mx_raise("tcp_connect failed to create socket");
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        closesocket(s);
        mx_raise("tcp_connect failed to connect to %s:%d", host, port);
    }
    int slot = mx_alloc_sock_slot(s);
    if (slot < 0) {
        closesocket(s);
        mx_raise("tcp_connect: too many open sockets");
    }
    return mx_int(slot);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) mx_raise("tcp_connect failed to create socket: %s", strerror(errno));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(s);
        mx_raise("tcp_connect failed to connect to %s:%d: %s", host, port, strerror(errno));
    }
    int slot = mx_alloc_sock_slot(s);
    if (slot < 0) {
        close(s);
        mx_raise("tcp_connect: too many open sockets");
    }
    return mx_int(slot);
#endif
}

/* mx_try_native - the dispatcher. See the note above for why it is one function
 * and why it returns an int. */
int mx_try_native(const char *name, MxValue *a, int n, MxValue *out) {
    if (!name || !name[0]) return 0;
    MxArray *r;
    switch (name[0]) {
        case 'a': {
            if (strcmp(name, "aot_get_prog") == 0) {
                if (n != 1 || a[0].tag != MX_STR)
                    mx_raise("aot_get_prog expects 1 module name string");
                MxValue prog = mx_aot_get_prog(a[0].as.s);
                *out = prog;
                return 1;
            }
            break;
        }
        case 's': {
            /* --- sys --- */
            if (strcmp(name, "sys_args") == 0) {
                int i, k = 0;
                if (n != 0) mx_raise("sys_args expects 0 arguments");
                int count = (g_argc > 1) ? (g_argc - 1) : 0;
                r = mx_arr_new(count > 0 ? count : 1);
                for (i = 1; i < g_argc; i++) r->v[k++] = mx_str(g_argv[i]);
                r->n = (size_t)k;
                *out = mx_arr_value(r);
                return 1;
            }
            if (strcmp(name, "sys_exit") == 0) {
                if (n != 1 || !mx_is_num(a[0]))
                    mx_raise("sys_exit expects 1 numeric exit code");
                fflush(stdout);
                fflush(stderr);
                exit((int)a[0].as.i);
            }
            if (strcmp(name, "sleep_ms") == 0) {
                if (n != 1 || !mx_is_num(a[0]))
                    mx_raise("sleep_ms expects 1 number of milliseconds");
#ifdef _WIN32
                if (a[0].as.i > 0) {
                    Sleep((DWORD)a[0].as.i);
                }
#else
                struct timespec ts;
                if (a[0].as.i > 0) {
                    ts.tv_sec = a[0].as.i / 1000;
                    ts.tv_nsec = (a[0].as.i % 1000) * 1000000L;
                    nanosleep(&ts, NULL);
                }
#endif
                *out = mx_null();
                return 1;
            }
            if (strcmp(name, "sys_exec") == 0) {
                if (n != 1 || a[0].tag != MX_STR)
                    mx_raise("sys_exec expects 1 command string");
                FILE *fp;
#ifdef _WIN32
                fp = _popen(a[0].as.s, "r");
#else
                fp = popen(a[0].as.s, "r");
#endif
                if (!fp) mx_raise("sys_exec failed to run command");
                size_t cap = 1024, len = 0;
                char *buf = (char *)malloc(cap);
                if (!buf) {
#ifdef _WIN32
                    _pclose(fp);
#else
                    pclose(fp);
#endif
                    mx_raise("sys_exec out of memory");
                }
                char chunk[512];
                while (fgets(chunk, sizeof(chunk), fp) != NULL) {
                    size_t clen = strlen(chunk);
                    if (len + clen + 1 > cap) {
                        cap = (cap * 2 > len + clen + 1) ? cap * 2 : len + clen + 1024;
                        char *nb = (char *)realloc(buf, cap);
                        if (!nb) {
                            free(buf);
#ifdef _WIN32
                            _pclose(fp);
#else
                            pclose(fp);
#endif
                            mx_raise("sys_exec out of memory");
                        }
                        buf = nb;
                    }
                    memcpy(buf + len, chunk, clen);
                    len += clen;
                }
                buf[len] = '\0';
#ifdef _WIN32
                int status = _pclose(fp);
                int code = status;
#else
                int status = pclose(fp);
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
                r = mx_arr_new(2);
                r->v[0] = mx_int(code);
                r->v[1] = mx_str(arena_strdup(buf, len));
                r->n = 2;
                free(buf);
                *out = mx_arr_value(r);
                return 1;
            }
            if (strcmp(name, "str_ord") == 0) {
                if (n != 1 || a[0].tag != MX_STR || strlen(a[0].as.s) != 1)
                    mx_raise("str_ord expects 1 single-character string");
                *out = mx_int((unsigned char)a[0].as.s[0]);
                return 1;
            }
            if (strcmp(name, "str_chr") == 0) {
                if (n != 1 || !mx_is_num(a[0]))
                    mx_raise("str_chr expects 1 integer");
                int64_t code = a[0].as.i;
                if (code < 0 || code > 255)
                    mx_raise("str_chr expects an integer in range 0..255");
                if (!g_ascii_chars_inited) mx_init_ascii_chars();
                *out = mx_str(g_ascii_chars[(unsigned char)code]);
                return 1;
            }
            break;
        }
        case 't': {
            /* --- time --- */
            if (strcmp(name, "time_now_ms") == 0) {
                if (n != 0) mx_raise("time_now_ms expects 0 arguments");
#ifdef _WIN32
                FILETIME ft;
                ULARGE_INTEGER uli;
                GetSystemTimeAsFileTime(&ft);
                uli.LowPart = ft.dwLowDateTime;
                uli.HighPart = ft.dwHighDateTime;
                int64_t ms = (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10000ULL);
                *out = mx_int(ms);
#else
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                *out = mx_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
                return 1;
            }
            /* --- tcp sockets --- */
            if (strcmp(name, "tcp_listen") == 0)  { *out = mx_tcp_listen_impl(a, n);  return 1; }
            if (strcmp(name, "tcp_accept") == 0)  { *out = mx_tcp_accept_impl(a, n);  return 1; }
            if (strcmp(name, "tcp_recv") == 0)    { *out = mx_tcp_recv_impl(a, n);    return 1; }
            if (strcmp(name, "tcp_send") == 0)    { *out = mx_tcp_send_impl(a, n);    return 1; }
            if (strcmp(name, "tcp_close") == 0)   { *out = mx_tcp_close_impl(a, n);   return 1; }
            if (strcmp(name, "tcp_connect") == 0) { *out = mx_tcp_connect_impl(a, n); return 1; }
            break;
        }
        case 'o': {
            /* --- os --- */
            if (strcmp(name, "os_sep") == 0) {
                if (n != 0) mx_raise("os_sep expects 0 arguments");
#ifdef _WIN32
                *out = mx_str("\\");
#else
                *out = mx_str("/");
#endif
                return 1;
            }
            if (strcmp(name, "os_cwd") == 0) {
                char buf[4096];
                if (n != 0) mx_raise("os_cwd expects 0 arguments");
                if (!mx_os_getcwd(buf, sizeof buf)) mx_raise("os_cwd: %s", strerror(errno));
                *out = mx_str(arena_strdup(buf, strlen(buf)));
                return 1;
            }
            if (strcmp(name, "os_env") == 0) {
                const char *v;
                if (n != 1 || a[0].tag != MX_STR)
                    mx_raise("os_env expects 1 string argument");
                v = getenv(a[0].as.s);
                *out = v ? mx_str(v) : mx_null();
                return 1;
            }
            if (strcmp(name, "os_listdir") == 0) {
                /* SORTED, because the host sorts (`sorted(_os.listdir(...))`) and the
                 * order is observable: the import walk and os_listdir consumers both
                 * depend on it. readdir's order is filesystem order, which differs
                 * between machines and between runs. */
                char **names = NULL;
                size_t cnt = 0, cap = 0, i, j;
                if (n != 1 || a[0].tag != MX_STR)
                    mx_raise("os_listdir expects 1 string argument");
#ifdef _WIN32
                char search_path[1050];
                snprintf(search_path, sizeof search_path, "%s\\*", a[0].as.s);
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA(search_path, &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                            continue;
                        if (cnt >= cap) {
                            size_t ncap = cap ? cap * 2 : 32;
                            char **new_names = (char **)arena_alloc(ncap * sizeof(char *));
                            if (names && cnt) memcpy(new_names, names, cnt * sizeof(char *));
                            names = new_names;
                            cap = ncap;
                        }
                        names[cnt++] = arena_strdup(fd.cFileName, strlen(fd.cFileName));
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                } else {
                    mx_raise("os_listdir: %s", strerror(errno));
                }
#else
                DIR *d;
                struct dirent *e;
                d = opendir(a[0].as.s);
                if (!d) mx_raise("os_listdir: %s", strerror(errno));
                while ((e = readdir(d)) != NULL) {
                    if (cnt >= cap) {
                        size_t ncap = cap ? cap * 2 : 32;
                        char **new_names = (char **)arena_alloc(ncap * sizeof(char *));
                        if (names && cnt) memcpy(new_names, names, cnt * sizeof(char *));
                        names = new_names;
                        cap = ncap;
                    }
                    names[cnt++] = arena_strdup(e->d_name, strlen(e->d_name));
                }
                closedir(d);
#endif
                for (i = 1; i < cnt; i++) {
                    char *key = names[i];
                    for (j = i; j > 0 && strcmp(names[j - 1], key) > 0; j--)
                        names[j] = names[j - 1];
                    names[j] = key;
                }
                r = mx_arr_new(cnt ? cnt : 1);
                for (i = 0; i < cnt; i++) r->v[i] = mx_str(names[i]);
                r->n = cnt;
                *out = mx_arr_value(r);
                return 1;
            }
            break;
        }
        case 'm': {
            /* --- math --- */
            if (strcmp(name, "math_floor") == 0) {
                if (n != 1 || !mx_is_num(a[0])) mx_raise("math_floor expects 1 number");
                *out = (a[0].tag == MX_INT) ? a[0] : mx_int((int64_t)floor(a[0].as.d));
                return 1;
            }
            /* --- version --- */
            if (strcmp(name, "mx_version") == 0) {
                if (n != 0) mx_raise("mx_version expects 0 arguments");
                *out = mx_str(MX_VERSION);
                return 1;
            }
            break;
        }
        case 'i': {
            if (strcmp(name, "idiv") == 0) {
                if (n != 2 || !mx_is_num(a[0]) || !mx_is_num(a[1]))
                    mx_raise("idiv expects 2 numbers");
                if ((a[1].tag == MX_INT && a[1].as.i == 0) || (a[1].tag == MX_NUM && a[1].as.d == 0.0))
                    mx_raise("idiv: division by zero");
                *out = mx_idiv(a[0], a[1]);
                return 1;
            }
            break;
        }
        case 'd': {
            /* --- db_* : libsqlite3, for real (CB-E retracted; the library is here) --- */
            if (strcmp(name, "db_open") == 0)  { *out = db_open_n(a, n);  return 1; }
            if (strcmp(name, "db_close") == 0) { *out = db_close_n(a, n); return 1; }
            if (strcmp(name, "db_exec") == 0)  { *out = db_exec_n(a, n);  return 1; }
            if (strcmp(name, "db_query") == 0) { *out = db_query_n(a, n); return 1; }
            break;
        }
        case 'h': {
            /* --- net : HTTP client via curl --- */
            if (strcmp(name, "http_get") == 0) {
                if (n != 1 || a[0].tag != MX_STR)
                    mx_raise("http_get expects 1 string argument");
                *out = mx_http_get_impl(a[0].as.s);
                return 1;
            }
            if (strcmp(name, "http_post") == 0) {
                if (n < 2 || n > 3 || a[0].tag != MX_STR || a[1].tag != MX_STR || (n == 3 && a[2].tag != MX_STR))
                    mx_raise("http_post expects (url, body, [content_type])");
                const char *url = a[0].as.s;
                const char *body = a[1].as.s;
                size_t body_len = a[1].tag == MX_STR ? strlen(body) : 0;
                const char *ctype = (n == 3) ? a[2].as.s : "application/x-www-form-urlencoded";
                *out = mx_http_post_impl(url, body, body_len, ctype);
                return 1;
            }
            break;
        }
        default:
            return 0;
    }
    return 0;
}

/* ---- the FILE natives (B-3.3) ------------------------------------------
 *
 * Both entry points are EXPORTED (not static) because generated code names
 * them. Static would make them unused in the AST-only build - where nothing
 * calls them, because an AST-only build runs corpus files through the AST half's
 * own eval_file_builtin - and G6 refuses an unused-function warning.
 *
 * readx / writex / appendx / exportx, transcribed from eval_file_builtin
 * (interpreter.py:903-927). They are ONE function with a `kind` parameter
 * because the host's is one function with a `kind` parameter - and because the
 * parts they share are the interesting ones: `display` for the text that gets
 * written (never str()), the value RETURNED being the second argument rather
 * than a boolean, and the OSError handler, which is where the host's message
 * shape lives.
 *
 * The message shape is the non-obvious part: `f"{kind.lower()}x: {e}"`, so
 * Read becomes "readx: ..." and Write "writex: ...". It interpolates PYTHON's
 * OSError text, which this cannot reproduce exactly, so what it interpolates is
 * the C equivalent - strerror - under the same prefix. That is a KNOWN wording
 * difference, not a silent one: no corpus case asserts a strerror string,
 * because the host's half of it is Python's. The PREFIX is exact, and the prefix
 * is what a reader recognises. */
/* NOT guarded: the AST half's N_CALL arm calls it by name, and the generated
 * path calls it by name, so an AST-only build uses it too. An earlier #ifdef
 * MX_BOTH here was wrong twice over - it excluded the build that needs it and
 * admitted nothing the other builds lack. */
MxValue mx_file_builtin(const char *kind, MxValue *args, int nargs) {
    char path[1024];
    char buf[65536];
    if (nargs < 1)
        mx_raise("%s expects at least 1 argument(s)", kind);
    if (args[0].tag != MX_STR)
        mx_raise("%s expects a string, got %s", kind, mx_type_name(args[0]));
    snprintf(path, sizeof path, "%s", args[0].as.s);
    if (strcmp(kind, "readx") == 0) {
        FILE *f = fopen(path, "rb");
        if (!f) mx_raise("readx: [Errno %d] %s", errno, strerror(errno));
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) sz = 0;
        char *content = (char *)arena_alloc((size_t)sz + 1);
        size_t n = fread(content, 1, (size_t)sz, f);
        fclose(f);
        content[n] = '\0';
        return mx_str(content);
    }
    if (strcmp(kind, "exportx") == 0) {
        /* The host JSON-encodes (json.dump), so exportx is a STRUCTURAL dump,
         * not a display. A compiled program cannot reimplement json.dump in C
         * for every value shape, so this emits the DISPLAY text and says so in
         * the comment rather than pretending. interp.mx's one use is a trace
         * file, and the corpus has no exportx golden - which is exactly why the
         * difference is recorded here instead of discovered by a reader. */
        FILE *f = fopen(path, "wb");
        if (!f) mx_raise("exportx: [Errno %d] %s", errno, strerror(errno));
        mx_display(nargs > 1 ? args[1] : mx_null(), buf, sizeof buf);
        fputs(buf, f);
        fclose(f);
        return nargs > 1 ? args[1] : mx_null();
    }
    {
        /* writex and appendx differ only in the mode, and both write DISPLAY of
         * the second argument and RETURN it. */
        int append = strcmp(kind, "appendx") == 0;
        FILE *f;
        if (nargs < 2)
            mx_raise("%s expects 2 argument(s), got %d", kind, nargs);
        f = fopen(path, append ? "ab" : "wb");
        if (!f) mx_raise("%s: [Errno %d] %s", kind, errno, strerror(errno));
        mx_display(args[1], buf, sizeof buf);
        fputs(buf, f);
        fclose(f);
        return args[1];
    }
}

/* mx_input - the inputx prompt, delegated the way the host delegates it
 * (interpreter.py:520-524 calls input()).
 *
 * EOF is the interesting half and the file says so at CF_EOF: the host renders
 * it TWO ways - bare when a catchx sees it, prefixed with "EOFError: " when it
 * escapes to main. So the substrate raises the BARE form and lets the uncaught
 * path add its prefix, which is the only way one function can satisfy both. */
MxValue mx_input(MxValue prompt) {
    char buf[4096];
    if (prompt.tag == MX_STR && prompt.as.s && *prompt.as.s) {
        fputs(prompt.as.s, stdout);
        fflush(stdout);
    }
    if (!fgets(buf, sizeof buf, stdin)) mx_raise("EOF when reading a line");
    {
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = '\0';
        return mx_str(arena_strdup(buf, strlen(buf)));
    }
}

/* ---- eval_dot - the FOUR shapes and the FOUR error texts (B-3.3) --------
 *
 * The host (interpreter.py:596-624) dispatches on the OBJECT'S shape, in this
 * order, and the order is the specification:
 *
 *   1. MODULE   funcs, then classes, then lets. THREE SEPARATE DICTS, checked
 *               in that order (:599-605). A module that exports a function and
 *               a let of the same name resolves to the FUNCTION - so this is
 *               three ordered lookups, not one merged table.
 *   2. INSTANCE  **FIELDS FIRST, THEN THE CLASS CHAIN** (:607-613). This is
 *               the I-M4 shadowing rule and it is the single most surprising
 *               line in the file: a FIELD with the same name as a METHOD wins,
 *               because the fields dict is consulted before any method lookup.
 *               Nobody would guess it, which is why the host's own comment
 *               flags it - and why a port that "helpfully" checked methods first
 *               passes every simple case and breaks the moment a field and a
 *               method share a name. Case 21 pins it.
 *   3. SUPER     chain only, NO field lookup (:614-618), because a super box is
 *               a method namespace: `super.init` must find the PARENT's init
 *               even when the receiver has a field of that name.
 *   4. anything else raises "Cannot access field '<f>' on <type>".
 *
 * A bound method is returned, not called: the CALL happens at eval_call, which
 * supplies the instance and the DEFINING class (the `klass: k` in :612-613 is
 * the class that DEFINED the method, not the receiver's class - that is what
 * makes a multi-level super chain resolve). */
/* mx_eval_new - resolution BEFORE arguments (COMPILE_DESIGN 7.4).
 *
 * The host resolves the class, THEN evaluates the arguments
 * (interpreter.py:637-649): the two texts
 *   "Class 'C' not defined"
 *   "Module 'm' has no class 'C'"
 * therefore win over anything the ARGUMENTS would raise. `newx Missing(f())`
 * reports the missing class, not f()'s error - and a port that evaluated the
 * args first would report f()'s, which is a different message for the same
 * program. Case 24 pins the order.
 *
 * Then, in the host's order:
 *   1. class_chain - validates unknown parents and cycles (:651)
 *   2. fields merged PARENT FIRST, child overriding (:653-656). Reversed(chain)
 *      then dict assignment means the child wins, which is the opposite of what
 *      iterating the chain forward would give.
 *   3. the NEAREST init up the chain runs - an inherited init runs if the class
 *      defines none (:659-663). "Nearest" means the first found walking UP, so
 *      a subclass's own init wins and an inherited one is the fallback.
 *   4. args with no init at all is an error (:665-666), not a silent no-op. */
static MxValue mx_eval_new(MxNode *n, MxEnv *env) {
    MxNode *klass = NULL;
    MxNode *chain[64];
    MxDict *fields;
    MxInstance *inst;
    MxValue *argv;
    int nargs = 0, nchain, i, j;

    /* 1. resolve, before any argument is evaluated */
    klass = mx_resolve_class(n->class_name);
    if (!klass) {
        const char *dot = strrchr(n->class_name, '.');
        if (dot) {
            char modname[128];
            size_t ml = (size_t)(dot - n->class_name);
            if (ml < sizeof modname) {
                MxValue mod;
                memcpy(modname, n->class_name, ml);
                modname[ml] = '\0';
                mod = env_get_or_null(g_globals, modname);
                if (mod.tag == MX_MOD) {
                    MxModule *m = (MxModule *)mod.as.box;
                    MxValue c = mx_dict_get(m->classes, dot + 1);
                    if (c.tag == MX_FUNC || c.tag == MX_ARR) {
                        klass = (MxNode *)c.as.box;
                    }
                }
            }
            if (!klass)
                mx_raise("Module '%s' has no class '%s'", modname, dot + 1);
        } else {
            mx_raise("Class '%s' not defined", n->class_name);
        }
    }

    /* 2. arguments, evaluated left to right */
    argv = (MxValue *)arena_alloc((size_t)(n->nargs ? n->nargs : 1) * sizeof(MxValue));
    for (i = 0; i < n->nargs; i++) argv[nargs++] = mx_eval(n->args[i], env);

    /* 3. validate the chain (raises on unknown parent / cycle) */
    nchain = mx_class_chain(klass, chain);
    if (nchain < 0) return mx_null();

    /* 4. fields merged PARENT FIRST, child overriding - reversed(chain) with
     * last-write-wins is exactly what makes the CHILD win */
    fields = mx_dict_new(8);
    for (i = nchain - 1; i >= 0; i--) {
        for (j = 0; j < chain[i]->nfields; j++) {
            mx_dict_set(fields, chain[i]->fields[j]->name,
                        mx_eval(chain[i]->fields[j]->a, env));
        }
    }

    inst = (MxInstance *)arena_alloc(sizeof(MxInstance));
    inst->stamp = mx_next_stamp();
    inst->klass = klass;
    inst->fields = fields;
    inst->class_name = klass->name;

    /* 5. the NEAREST init up the chain */
    {   MxNode *init_class = NULL;
        MxNode *init_fn = NULL;
        for (i = 0; i < nchain; i++) {
            if (chain[i]->init) { init_class = chain[i]; init_fn = chain[i]->init; break; }
        }
        if (init_fn) {
            MxValue r = mx_call_instance(init_fn, argv, nargs, inst, init_class);
            mx_clear_signal();
            (void)r;
        } else if (nargs > 0) {
            mx_raise("Class '%s' has no init but %d argument(s) were given",
                     klass->name, nargs);
            return mx_null();
        }
    }
    {   MxValue v;
        v.tag = MX_INST; v.as.box = inst;
        return v;
    }
}

static MxValue mx_eval_dot(MxValue obj, const char *field) {
    if (obj.tag == MX_MOD) {
        /* 1. MODULE: funcs, then classes, then lets - THREE ordered lookups
         * (:599-605). Not one merged table: a function and a let sharing a name
         * resolve to the FUNCTION, and case 22 pins that. */
        MxModule *m = (MxModule *)obj.as.box;
        if (mx_dict_has(m->funcs, field))   return mx_dict_get(m->funcs, field);
        if (mx_dict_has(m->classes, field)) return mx_dict_get(m->classes, field);
        if (mx_dict_has(m->lets, field))    return mx_dict_get(m->lets, field);
        mx_raise("Module '%s' has no attribute '%s'", m->mod_name, field);
        return mx_null();
    }
    if (obj.tag == MX_INST) {
        MxInstance *inst = (MxInstance *)obj.as.box;
        MxNode *chain[MX_CHAIN_MAX];
        int n, i;
        /* 2. INSTANCE: FIELDS FIRST (:608). Then the class chain for a method.
         * This order is the I-M4 shadowing rule and it is the one line nobody
         * would guess - see the note above. */
        if (mx_dict_has(inst->fields, field)) return mx_dict_get(inst->fields, field);
        n = mx_class_chain(inst->klass, chain);
        if (n < 0) return mx_null();          /* the cycle/unknown error raised */
        for (i = 0; i < n; i++) {
            MxNode *m = mx_find_method(chain[i], field);
            if (m) {
                MxBoundMethod *bm = (MxBoundMethod *)arena_alloc(sizeof(MxBoundMethod));
                bm->stamp = mx_next_stamp();
                bm->method = m;
                bm->instance = inst;
                bm->klass = chain[i];        /* the DEFINING class, not the
                                               * receiver's - that is what makes
                                               * a multi-level super chain
                                               * resolve (:612) */
                {   MxValue v;
                    v.tag = MX_METHOD; v.as.box = bm; return v;
                }
            }
        }
        mx_raise("Field/method '%s' not found on %s", field, inst->class_name);
        return mx_null();
    }
    if (obj.tag == MX_SUPER) {
        /* 3. SUPER: the chain only, NO field lookup (:614-618). A super box is a
         * METHOD namespace, so `super.init` finds the parent's init even when the
         * receiver carries a field of that name. */
        MxSuper *sp = (MxSuper *)obj.as.box;
        MxNode *chain[MX_CHAIN_MAX];
        int n, i;
        n = mx_class_chain(sp->klass, chain);
        if (n < 0) return mx_null();
        for (i = 0; i < n; i++) {
            MxNode *m = mx_find_method(chain[i], field);
            if (m) {
                MxBoundMethod *bm = (MxBoundMethod *)arena_alloc(sizeof(MxBoundMethod));
                bm->stamp = mx_next_stamp();
                bm->method = m;
                bm->instance = sp->instance;
                bm->klass = chain[i];
                {   MxValue v;
                    v.tag = MX_METHOD; v.as.box = bm; return v;
                }
            }
        }
        mx_raise("super has no method '%s'", field);
        return mx_null();
    }
    /* 4. everything else - and the TEXT names the TYPE, which is why this is
     * the last arm and not an early one: `Cannot access field 'x' on int` for
     * an int, on a function, and on a module that had no such attribute. */
    mx_raise("Cannot access field '%s' on %s", field, mx_type_name(obj));
    return mx_null();
}

static MxNode *node(NTag t) {
    MxNode *n = (MxNode *)arena_alloc(sizeof(MxNode));
    memset(n, 0, sizeof *n);
    n->tag = t;
    return n;
}

/* ast_new_by_name - see the declaration for why this exists. It SYNTHESISES an
 * N_NEW node carrying the already-evaluated arguments as literals, and calls
 * mx_eval_new with it.
 *
 * The arguments are put in as literals rather than re-wrapped in expression
 * nodes because they are already VALUES: evaluating them again would run their
 * side effects twice. A caller-count mismatch is impossible here - the array is
 * built with exactly nargs entries - so the arity check in mx_eval_new passes
 * for the same reason it would for a source-level `newx`.
 */
#ifdef MX_BOTH
static MxValue ast_new_by_name(const char *class_name, MxValue *args, int nargs) {
    MxNode *kn = mx_resolve_class(class_name);
    MxNode *nn;
    int i;
    if (!kn) mx_raise("Class '%s' not defined", class_name);
    nn = node(N_NEW);
    nn->line = 0;
    nn->class_name = arena_strdup(class_name, strlen(class_name));
    nn->nargs = nargs;
    nn->args = (MxNode **)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(MxNode *));
    for (i = 0; i < nargs; i++) {
        MxNode *lit = node(N_LITERAL);
        lit->lit = args[i];
        nn->args[i] = lit;
    }
    return mx_eval_new(nn, g_globals);
}
#endif /* MX_BOTH - only the generated path's mx_new_multi calls this */


/* Refusal for anything outside the subset. NOT a silent no-op: a construct
 * mini.c does not implement must fail loudly, or a corpus case that happens to
 * avoid it would pass by never being called (KICKOFF.md §1). */
static void mx_unsupported(const char *what, int line) {
    mx_raise("Unknown stmt %s", what);
    (void)line;
}

/* ===================================================================== */
/* 6. Lexer - the subset's token kinds only.                            */
/* ===================================================================== */

typedef enum { T_EOF, T_IDENT, T_INT, T_NUM, T_STR, T_KW, T_PUNCT } TType;

typedef struct {
    TType type;
    /* The token TEXT is an ARENA pointer, not a 128-byte array.
     *
     * It was a fixed array, and the crown found the ceiling: lexer.mx's escape
     * table is a single 1,536-character string literal, so the compiled
     * interpreter could not import the lexer port - the very module a guest
     * needs - and reported "string literal too long". B-3.2 had already had to
     * grow the token LIST and parse_body's statement array for the same reason,
     * and this is the third of the same family.
     *
     * A pointer rather than a bigger array because the array was per TOKEN and
     * the long literal is per FILE: sizing it to 2 KB would cost 40 MB on
     * interp.mx, whose tens of thousands of tokens are all short identifiers.
     * The arena already owns the source's lifetime, so a borrowed pointer costs
     * nothing and cannot dangle. */
    const char *text;
    int64_t i;
    double d;
    int  line;
} Token;

/* The corpus's keyword table. It grew with the corpus: `if`/`else`/`while`/
 * `break`/`continue` arrived at B-3.2, and their absence was a genuinely
 * confusing failure - the words lexed as IDENT, so parse_stmt never matched and
 * the parser reported "Unexpected token {" on the BLOCK, naming a brace that was
 * perfectly correct and hiding the real cause three tokens earlier. A missing
 * keyword entry is therefore not a small omission: it produces a message that
 * points away from itself. */
static const char *KEYWORDS[] = {
    "let", "printx", "true", "false", "nullx", "throwx", "tryx", "catchx",
    /* The null ALIASES: AGENTS.md records `null/nullx/nil` as interchangeable
     * spellings and lexer.py lexes all three as NULL. Only `nullx` was here, so
     * `let x = nil` lexed `nil` as an IDENT - and the failure named exactly the
     * word that was CORRECT (`Undefined variable 'nil'`), the same
     * points-away-from-itself shape the note above warns about. It reached the
     * crown too: module sources are parsed by this parser during the bootstrap,
     * so parser.mx's own seven `let v = nil` lines became Ident nodes and
     * evaluating any of them threw inside a loaded module. */
    "nil", "null",
    "return", "func", "mountx", "unmountx", "typeofx",
    "if", "else", "while", "loop", "break", "continue",
    "for", "in",
    /* B-3.3: the class keywords. `extends` is a separate keyword so the parent
     * name is optional by GRAMMAR rather than by a lookahead hack - the same
     * reason `else` is one (B-3.2). */
    "classx", "extends", "newx", "importx", "this", "super", NULL
};

static int is_kw(const char *s) {
    if (!s) return 0;
    int i;
    for (i = 0; KEYWORDS[i]; i++) {
        if (KEYWORDS[i] == s || (KEYWORDS[i][0] == s[0] && strcmp(KEYWORDS[i], s) == 0)) return 1;
    }
    return 0;
}

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_cont(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* ---- the two SAFETY NETS (B-3.5) ----------------------------------------
 *
 * A substrate that can freeze the machine it runs on is not a substrate, it is
 * a hazard, and the crown run proved it: a runaway loop consumed all RAM and the
 * machine died. Correctness work and safety work are different things, and this
 * is the latter - it has to be in place BEFORE the next correctness attempt,
 * because the next bug is unknown and may be a hang.
 *
 * 1. A WALL-CLOCK LIMIT, by SIGALRM.
 *
 *    A timer rather than a counter inside the evaluator, and that choice is the
 *    whole point: a check in the eval loop catches an interpreted infinite loop
 *    and nothing else. It would not catch a loop in the LEXER (which is where
 *    the freeze happened), or in the parser, or in the generated code, or in a
 *    single C helper that never returns. A signal interrupts all of them,
 *    because none of them can be spinning without the kernel delivering it.
 *
 *    The handler does NOT raise. mx_raise is a longjmp, and longjmp out of a
 *    signal handler lands in whatever frame happened to be current - which is
 *    precisely how a safety net becomes the crash. It writes the message and
 *    _exit()s, which cannot return to anything.
 *
 *    Exit 124 is GNU timeout's code for "timed out", chosen so an external
 *    watchdog and this one agree on what happened.
 *
 * 2. THE ARENA CEILING, above - an allocation that grows without bound is the
 *    same freeze by a different route.
 *
 * Both limits are environment-overridable (MOUNTX_EXEC_LIMIT, MOUNTX_ARENA_MAX)
 * because a limit nobody can lower in a test is a limit nobody can prove fires.
 */
#define MX_EXEC_LIMIT_DEFAULT 300
static int g_exec_limit = MX_EXEC_LIMIT_DEFAULT;

#ifndef _WIN32
static void mx_on_alarm(int sig) {
    (void)sig;
    /* write() not printf: this runs in a signal handler, where stdio's internal
     * lock may be held by the very code we just interrupted. */
    {
        static const char pre[] = "[Runtime] Execution timeout after ";
        char buf[64];
        int n = 0, v = g_exec_limit;
        char digits[8];
        int nd = 0;
        if (v <= 0) v = 0;
        do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (nd > 0) buf[n++] = digits[--nd];
        buf[n++] = ' '; buf[n++] = 's'; buf[n++] = '\n';
        ssize_t ignored = write(1, pre, sizeof pre - 1);
        ignored = write(1, buf, (size_t)n);
        ignored = write(1, "", 1);
        (void)ignored;
    }
    _exit(124);
}
#endif

/* mx_install_safety_nets - called by BOTH entry points (the AST half's main and
 * the EMITTED main), because a generated program's main is a different function
 * and a net installed in only one of them protects only half the ways in. */
void mx_install_safety_nets(void) {
    const char *e;
    if ((e = getenv("MOUNTX_EXEC_LIMIT")) != NULL && *e) {
        long v = strtol(e, NULL, 10);
        if (v >= 0) g_exec_limit = (int)v;
    }
    if ((e = getenv("MOUNTX_ARENA_MAX")) != NULL && *e) {
        long long v = atoll(e);
        if (v > 0) g_arena_max = (size_t)v;
    }
#ifndef _WIN32
    if (g_exec_limit > 0) {
        struct sigaction sa;
        struct itimerval it;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = mx_on_alarm;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGALRM, &sa, NULL);
        it.it_value.tv_sec = g_exec_limit;
        it.it_value.tv_usec = 0;
        it.it_interval.tv_sec = 0;          /* one shot: a limit, not a heartbeat */
        it.it_interval.tv_usec = 0;
        setitimer(ITIMER_REAL, &it, NULL);
    }
#endif
}

/* ---- G2: string ESCAPES (lexer.py:88-116) ------------------------------
 *
 * The scanner copies a literal VERBATIM and this decodes it, because a lexer
 * that stored the raw text would make "\n" two characters and "\u4e2d" six -
 * and every string comparison in a program would then be wrong in a way no
 * syntax error ever announces.
 *
 * The escape SET is the host's exactly: \\ \" / b f n r t. What is NOT in it
 * matters as much: \d, \p and \w are absent, so the host raises on them - and
 * all three DO appear in lexer.mx, inside COMMENTS. A comment is skipped before
 * any of this runs, which is why that file lexes at all; decoding escapes
 * outside a string literal would reject the very source it must read.
 *
 * The three error texts are the host's, quoting the RAW literal, because they
 * are compared as bytes. That is why decode_string takes the raw form rather
 * than a decoded prefix. */
static int mx_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* utf8_put - one code point as UTF-8, and its byte count. \uXXXX is four hex
 * digits, so the range is the BMP and the widest case is three bytes. */
static size_t utf8_put(char *out, unsigned cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* decode_string - the RAW literal (quotes included, as the host's `value` is) to
 * its decoded bytes. A decoded string is never longer than its raw form except
 * for \uXXXX, which SHRINKS six bytes to at most three, so out_cap >= raw_len + 1
 * always suffices. */
static size_t decode_string(const char *raw, char *out, size_t out_cap) {
    size_t i, n = 0;
    for (i = 1; raw[i] && raw[i] != '"'; i++) {
        char c = raw[i];
        if (c != '\\') {
            if (n + 1 >= out_cap) mx_raise("string literal too long");
            out[n++] = c;
            continue;
        }
        {
            char esc = raw[i + 1];
            char r;
            if (esc == '\0')
                mx_raise("Invalid string escape: lone '\\' at end of '%s'", raw);
            if (esc == 'u') {
                unsigned cp = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    int h = mx_hexval(raw[i + 2 + k]);
                    if (raw[i + 2 + k] == '\0' || h < 0)
                        mx_raise("Invalid string escape: \\u must be followed by "
                                 "4 hex digits in '%s'", raw);
                    cp = cp * 16 + (unsigned)h;
                }
                if (n + 4 >= out_cap) mx_raise("string literal too long");
                n += utf8_put(out + n, cp);
                i += 5;                     /* the \u plus its four digits */
                continue;
            }
            switch (esc) {
                case '\\': r = '\\'; break;
                case '"':  r = '"';  break;
                case '/':  r = '/';  break;
                case 'b':  r = '\b'; break;
                case 'f':  r = '\f'; break;
                case 'n':  r = '\n'; break;
                case 'r':  r = '\r'; break;
                case 't':  r = '\t'; break;
                default:
                    mx_raise("Invalid string escape '\\%c' in '%s'", esc, raw);
                    return n;
            }
            if (n + 1 >= out_cap) mx_raise("string literal too long");
            out[n++] = r;
            i += 1;              /* the ESCAPE character, on top of the loop's
                                 * own step over the backslash. Without it a
                                 * "\\u" was read as an escaped backslash followed
                                 * by a BARE 'u', and lexer.mx - which uses exactly
                                 * that to write a literal backslash-u - failed
                                 * with "\\u must be followed by 4 hex digits". */
        }
    }
    out[n] = '\0';
    return n;
}

static Token *lex(const char *src) {
    size_t cap = 1024, n = 0;
    /* Sized from the SOURCE, not guessed. `cap * 3` is generous: the densest
     * token in the corpus is a single character, so 3x the byte count always
     * suffices. The fixed 1024 this replaces refused with "token buffer
     * exhausted", which would have stopped B-3 the moment it was pointed at
     * interp.mx - a 3,104-line file with tens of thousands of tokens. Growing
     * after the fact is possible, but measuring first is cheaper and cannot
     * fail, and it keeps the arena's copy count down.
     *
     * This is the SECOND fixed-capacity refusal removed today (parse_body's was
     * the first). Both read as "the corpus is small so this is fine", and both
     * were false the moment a case grew past them. */
    Token *toks;
    int line = 1;
    const char *p = src;
    cap = strlen(src) * 3 + 16;
    toks = (Token *)arena_alloc(cap * sizeof(Token));
    while (*p) {
        if (*p == '\n') { line++; p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (p[0] == '#') { while (*p && *p != '\n') p++; continue; }
        if (n + 1 >= cap) {
            /* Cannot happen with the size computed above - every source byte
             * becomes at most one token - so this is a genuine invariant
             * violation rather than a size limit, and it says so. */
            mx_raise("internal: token buffer under-sized for this source");
        }
        {   Token *t = &toks[n];
            memset(t, 0, sizeof *t);
            t->line = line;
            if (*p == '"') {
                size_t len = 0;
                /* TWO buffers, and the reason is the error texts: `raw` keeps
                 * the literal exactly as written, because decode_string quotes
                 * IT in its messages and the host's message quotes the source
                 * form, not the decoded one. One buffer would have made either
                 * the stored text wrong or the messages wrong, and only one of
                 * those fails loudly. */
                char *raw = (char *)arena_alloc(4096);
                char *out = (char *)arena_alloc(4096);
                raw[len++] = *p++;              /* the opening quote */
                while (*p && *p != '"') {
                    if (len + 1 >= 4096) mx_raise("string literal too long");
                    /* An ESCAPED quote is not the closing one. The host's STRING
                     * regex consumes `\\"` as a unit (lexer.py:45), so a scanner
                     * that stops at any quote ends the literal at the escaped
                     * one - and the remainder, `", "\""`, was then read as
                     * code and died on a bare backslash. This is the same class of
                     * bug as the `i += 1` above: the SCAN must skip an escape
                     * pair even though the DECODE is what gives it meaning. */
                    if (*p == '\\' && p[1]) {
                        raw[len++] = *p++;
                        raw[len++] = *p++;
                        continue;
                    }
                    raw[len++] = *p++;
                }
                if (*p != '"') mx_raise("unterminated string literal");
                p++;
                raw[len++] = '"';
                raw[len] = '\0';
                decode_string(raw, out, 4096);
                t->text = out;
                t->type = T_STR;
                n++;
                continue;
            }
            if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {
                char *end = NULL;
                int is_float = 0;
                const char *start = p;
                while (is_digit(*p)) p++;
                if (*p == '.') { is_float = 1; p++;
                                 while (is_digit(*p)) p++; }
                {   size_t len = (size_t)(p - start);
                    {   char *numtxt = (char *)arena_alloc(len + 1);
                    memcpy(numtxt, start, len); numtxt[len] = '\0';
                    t->text = numtxt;
                }
                }
                if (is_float) { t->type = T_NUM; t->d = strtod(t->text, NULL); }
                else {
                    long long v = strtoll(t->text, &end, 10);
                    /* CB1: an integer literal that does not fit int64 is the
                     * SAME condition as an overflowing operation, so it gets
                     * the SAME frozen text (#72). */
                    if (errno == ERANGE) mx_raise("Integer overflow: result does not fit in 64 bits");
                    t->type = T_INT; t->i = (int64_t)v;
                }
                n++;
                continue;
            }
            if (is_ident_start(*p)) {
                size_t len = 0;
                char idt[256];
                while (is_ident_cont(*p)) {
                    if (len + 1 >= 256) mx_raise("identifier too long");
                    idt[len++] = *p++;
                }
                idt[len] = '\0';
                t->text = arena_strdup(idt, len);
                t->type = is_kw(t->text) ? T_KW : T_IDENT;
                n++;
                continue;
            }
            /* Punctuation, including `.` for field access. The ORDER above
             * matters and is not negotiable: the NUMBER test runs FIRST and
             * claims `.` whenever a digit follows it (1818), so `1.5` is a float
             * and `a.b` is a field access. Adding `.` to the punctuation set
             * BELOW that test therefore costs nothing - the two cases are
             * distinguished by the character AFTER the dot, which is exactly the
             * rule the host's lexer uses (TOKEN_SPEC's float pattern requires a
             * digit after the point).
             *
             * The set is a SUBSET of the host's, not the host's
             * full table - mini.c is the corpus's language, and the corpus is
             * the authority for what that language contains. '[' and ']' arrived
             * with the array value layer (B-3.1); ',' with the array literal's
             * element list. A character outside this set is a loud failure,
             * which is how B-1 found that it had no arrays at all: the corpus
             * case printed "Unexpected character '['". */
            /* TWO-CHARACTER operators, longest-match first. This has to happen
             * BEFORE the single-character set above, or `a == b` lexes as three
             * tokens (`=`, `=`, and then a bare ident) and the parser reports a
             * failure three tokens away from the cause - the same shape as the
             * `Expected ] at line 9` message that corpus case 14 produced.
             * The list is written longest-first on purpose: reading it in any
             * other order and "fixing" it later is the obvious mistake. */
            if (p[0] && p[1]) {
                const char *two[] = {"==", "!=", ">=", "<=", "&&", "||", NULL};
                int k, hit = 0;
                for (k = 0; two[k]; k++) {
                    if (p[0] == two[k][0] && p[1] == two[k][1]) {
                        {   char *pt = (char *)arena_alloc(3);
                            pt[0] = p[0]; pt[1] = p[1]; pt[2] = '\0';
                            t->text = pt;
                        }
                        t->type = T_PUNCT;
                        n++; p += 2;
                        hit = 1;
                        break;
                    }
                }
                /* `break` out of the inner loop, then `continue` the OUTER one -
                 * with BOTH, because the first draft used a bare `continue` here,
                 * which continues the `for (k)` loop, NOT the lexer. It re-tested
                 * the same two characters forever and then fell through into
                 * mx_raise, so every `==` reported "Unexpected character ' '" -
                 * the SPACE after the operator, which names neither the operator
                 * nor the real problem. A `continue` that means "next token" has
                 * to be written where the next token actually starts. */
                if (hit) continue;
            }
            if (strchr("<>", *p)) {
                {   char *pt = (char *)arena_alloc(2);
                    pt[0] = *p; pt[1] = '\0';
                    t->text = pt;
                }
                t->type = T_PUNCT;
                n++; p++;
                continue;
            }
            if (strchr("()[]{},+*/=->!<%.:", *p)) {
                {   char *pt = (char *)arena_alloc(2);
                    pt[0] = *p; pt[1] = '\0';
                    t->text = pt;
                }
                t->type = T_PUNCT;
                n++; p++;
                continue;
            }
            mx_raise("Unexpected character '%c'", *p);
            }
        }
    memset(&toks[n], 0, sizeof toks[n]);
    toks[n].type = T_EOF; toks[n].line = line;
    return toks;
}

/* ===================================================================== */
/* 7. Parser - the subset only.                                          */
/* ===================================================================== */

typedef struct P { Token *t; int i; } P;

static Token *peek(P *p) { return &p->t[p->i]; }
static Token *next(P *p) { return &p->t[p->i++]; }

static int at_punct(P *p, const char *s) {
    return peek(p)->type == T_PUNCT && strcmp(peek(p)->text, s) == 0;
}
static int at_kw(P *p, const char *s) {
    return peek(p)->type == T_KW && strcmp(peek(p)->text, s) == 0;
}
static void expect_punct(P *p, const char *s) {
    if (!at_punct(p, s)) mx_raise("Expected %s at line %d", s, peek(p)->line);
    p->i++;
}

static MxNode *parse_expr(P *p);
static MxNode *parse_unary(P *p);
static MxNode *parse_multiplicative(P *p);
static MxNode *parse_additive(P *p);
static MxNode *parse_logical_and(P *p);
static MxNode *parse_logical_or(P *p);

/* ast_op(sym) - the SYMBOL to the AST's NAME, at the point the token is
 * consumed. parser.py:262 stores op_tok.type ("PLUS", "EQEQ", ...), so the
 * ladder must produce those names, not the symbols. Doing it HERE rather than in
 * the lexer keeps tokenising and parsing separate: the lexer emits symbols and
 * the parser names them, which is the host's division too. */
static const char *ast_op(const char *sym) {
    static const char *tbl[][2] = {
        {"+", "PLUS"}, {"-", "MINUS"}, {"*", "MUL"}, {"/", "DIV"}, {"%", "MOD"},
        {"==", "EQEQ"}, {"!=", "NEQ"}, {">", "GT"}, {"<", "LT"},
        {">=", "GTE"}, {"<=", "LTE"}, {"&&", "AND"}, {"||", "OR"},
        {NULL, NULL}
    };
    int i;
    for (i = 0; tbl[i][0]; i++) if (strcmp(tbl[i][0], sym) == 0) return tbl[i][1];
    return "UNKNOWN";
}

/* op_at - is the next token this operator? A one-character peek, so it is
 * readable next to the ladder it serves. */
static int op_at(P *p, const char *sym) {
    return peek(p)->type == T_PUNCT && strcmp(peek(p)->text, sym) == 0;
}
/* The index-assignment target in parse_stmt reuses the EXPRESSION parser, so
 * these two must be visible above the statement parser. The dependency is real
 * rather than cosmetic: `a[i] = v` is a statement whose target is parsed by the
 * expression grammar. */
static MxNode *parse_primary(P *p);
static MxNode *parse_postfix(P *p, MxNode *left);
static MxNode *parse_stmt(P *p);

static MxNode *parse_body(P *p, const char *end_kw) {
    MxNode *prog = node(N_PROGRAM);
    int cap = 16, n = 0;
    MxNode **body = (MxNode **)arena_alloc(cap * sizeof(MxNode *));
    /* Stop on the end keyword OR a closing brace. A brace is what terminates an
     * ordinary block and a catch body; without this a `{ ... }` block swallows
     * the rest of the program, which is exactly what case 05 did. */
    while (!(end_kw && at_kw(p, end_kw))
           && !(peek(p)->type == T_PUNCT && strcmp(peek(p)->text, "}") == 0)
           && peek(p)->type != T_EOF) {
        /* The list GROWS. It used to REFUSE at 16 statements, on the reasoning
         * that "tokens are bounded by the corpus" - but a block's length is not
         * a property of the corpus, it is a property of the PROGRAM, and case 17
         * (thirty statements in one block) is a perfectly ordinary program. The
         * refusal reported "statement buffer exhausted", which reads like a bug
         * in the source rather than a limit in the reader.
         *
         * Growth allocates a FRESH list and copies, exactly like mx_arr_grow
         * did: realloc would move the block, and the arena never frees anyway,
         * so a move costs nothing in memory safety and costs a copy. */
        if (n >= cap) {
            int ncap = cap * 2;
            MxNode **nb = (MxNode **)arena_alloc((size_t)ncap * sizeof(MxNode *));
            int i;
            for (i = 0; i < cap; i++) nb[i] = body[i];
            body = nb;
            cap = ncap;
        }
        body[n++] = parse_stmt(p);
    }
    prog->body = body; prog->nbody = n; prog->line = peek(p)->line;
    return prog;
}

/* parse_stmt_expr_target - the TARGET side of an assignment: a postfix chain
 * starting from the current token. It exists so the statement parser can look
 * ahead for `target = value` without duplicating postfix parsing, and it is the
 * same function the expression path uses - one grammar, two callers. */
static MxNode *parse_stmt_expr_target(P *p) {
    return parse_expr(p);
}

static MxNode *parse(P *p) {
    MxNode *prog;
    if (peek(p)->type == T_KW && strcmp(peek(p)->text, "mountx") == 0) p->i++;
    prog = parse_body(p, NULL);
    if (peek(p)->type == T_KW && strcmp(peek(p)->text, "unmountx") == 0) p->i++;
    return prog;
}

static MxNode *parse_stmt(P *p) {
    Token *t = peek(p);
    if (t->type == T_KW) {
        if (strcmp(t->text, "let") == 0) {
            p->i++;
            MxNode *n = node(N_LET);
            Token *id = next(p);
            if (id->type != T_IDENT) mx_raise("Expected identifier after let at line %d", id->line);
            n->name = arena_strdup(id->text, strlen(id->text));
            expect_punct(p, "=");
            n->a = parse_expr(p);
            n->line = id->line;
            return n;
        }
        if (strcmp(t->text, "if") == 0) {
            /* `if cond { }` / `else { }` / `else if { }`.
             * An `else if` is parsed by RECURSING into this same branch, so it
             * becomes an N_IF in the else slot - which is exactly the host's
             * shape (parser.py:181 nests), and it is why a chain of any depth
             * needs no extra node kind or any special case here. */
            MxNode *n = node(N_IF);
            MxNode *blk;
            n->line = t->line;
            p->i++;                   /* consume the keyword */
            /* NO parentheses: the grammar is `if cond {` (parser.py:171), not
             * `if (cond) {`. The first draft expected them and reported
             * "Expected ( at line 3" on a correct program. */
            n->a = parse_expr(p);
            /* parse_body STOPS at the closing brace without eating it, so the
             * opening brace has to be consumed here. tryx does the same, and
             * forgetting it produces "Unexpected token {" naming the brace -
             * which is the correct token but the wrong place to look. */
            expect_punct(p, "{");
            blk = parse_body(p, NULL);
            n->body = blk->body;
            n->nbody = blk->nbody;
            expect_punct(p, "}");
            /* `else` is OPTIONAL and its absence is the difference between "falsy
             * and nothing to do" and "falsy and run this". Testing for the
             * keyword with a lookahead - not for a `{` - is what lets `else if`
             * be recognised, and an else-if is parsed by RECURSION into this same
             * branch, producing an N_IF in the else slot. That is the host's own
             * shape (parser.py:181 nests), so a chain costs one frame per link
             * and needs no node kind of its own. */
            if (at_kw(p, "else")) {
                p->i++;
                if (at_kw(p, "if")) {
                    n->else_body = parse_stmt(p);   /* the recursive link */
                } else {
                    expect_punct(p, "{");
                    n->else_body = parse_body(p, NULL);
                    expect_punct(p, "}");
                }
            }
            return n;
        }
        if (strcmp(t->text, "while") == 0) {
            MxNode *n = node(N_WHILE);
            n->line = t->line;
            p->i++;                   /* consume the keyword */
            n->a = parse_expr(p);      /* `while cond {`, no parentheses */
            expect_punct(p, "{");
            {   MxNode *blk = parse_body(p, NULL);
                n->body = blk->body;
                n->nbody = blk->nbody;
            }
            expect_punct(p, "}");
            return n;
        }
        if (strcmp(t->text, "loop") == 0) {
            /* `loop COUNT { }` - a COUNTED loop, not a conditional one
             * (parser.py:189-193, interpreter.py:326-335). It is NOT sugar for
             * `while`: the count is evaluated ONCE, a non-integer is an ERROR
             * rather than a truthiness test, and `loop 0` runs nothing while
             * `loop -1` also runs nothing (max(0, count)).
             *
             * It was missing here entirely, and the crown found it: the B-2
             * binary imports stdlib/text/string.mx, whose `repeat` is written
             * with `loop`, so the compiled interpreter could not import the very
             * module its guest needs. The token and the node kind were both
             * absent - not a wrong rule, a missing one. */
            MxNode *n = node(N_LOOP);
            n->line = t->line;
            p->i++;                   /* consume the keyword */
            n->a = parse_expr(p);      /* the count */
            expect_punct(p, "{");
            {   MxNode *blk = parse_body(p, NULL);
                n->body = blk->body;
                n->nbody = blk->nbody;
            }
            expect_punct(p, "}");
            return n;
        }
        if (strcmp(t->text, "for") == 0) {
            /* `for x in xs { }` and `for i, v in xs { }` (parser.py:195-205).
             *
             * TWO forms because the SECOND binding is an index: the host defines
             * `var` FIRST and `var2` SECOND as INDEX then ELEMENT
             * (interpreter.py:394-397), which reads backwards from the visual
             * order and is the kind of detail a transcription has to copy rather
             * than improve.
             *
             * `var2` is NULL for the one-binding form and that NULL is what the
             * evaluator branches on - so it must be stored as NULL, not as an
             * empty string, or `node.get("var2")` would be true and every
             * one-binding loop would try to bind a second name. */
            MxNode *n = node(N_FORIN);
            Token *id;
            n->line = t->line;
            p->i++;                                   /* consume `for` */
            id = next(p);
            if (!id || id->type != T_IDENT)
                mx_raise("Expected identifier after for at line %d", n->line);
            n->name = arena_strdup(id->text, strlen(id->text));
            n->var2 = NULL;
            if (peek(p) && strcmp(peek(p)->text, ",") == 0) {
                p->i++;                               /* consume the comma */
                id = next(p);
                if (!id || id->type != T_IDENT)
                    mx_raise("Expected identifier after ',' at line %d", n->line);
                n->var2 = arena_strdup(id->text, strlen(id->text));
            }
            if (!peek(p) || strcmp(peek(p)->text, "in") != 0)
                mx_raise("Expected 'in' after for variable at line %d", n->line);
            p->i++;                                   /* consume `in` */
            n->a = parse_expr(p);                     /* the iterable */
            expect_punct(p, "{");
            {   MxNode *blk = parse_body(p, NULL);
                n->body = blk->body;
                n->nbody = blk->nbody;
            }
            expect_punct(p, "}");
            return n;
        }
        if (strcmp(t->text, "break") == 0) {
            MxNode *n = node(N_BREAK);
            n->line = t->line;
            p->i++;                   /* consume the keyword */
            return n;
        }
        if (strcmp(t->text, "continue") == 0) {
            MxNode *n = node(N_CONTINUE);
            n->line = t->line;
            p->i++;                   /* consume the keyword */
            return n;
        }
        if (strcmp(t->text, "printx") == 0) {
            p->i++;
            MxNode *n = node(N_PRINT);
            expect_punct(p, "(");
            n->a = parse_expr(p);
            expect_punct(p, ")");
            n->line = t->line;
            return n;
        }
        if (strcmp(t->text, "throwx") == 0) {
            p->i++;
            MxNode *n = node(N_THROW);
            n->a = parse_expr(p);
            n->line = t->line;
            return n;
        }
        if (strcmp(t->text, "return") == 0) {
            p->i++;
            MxNode *n = node(N_RETURN);
            if (!at_punct(p, "}")) n->a = parse_expr(p);
            n->line = t->line;
            return n;
        }
        if (strcmp(t->text, "tryx") == 0) {
            p->i++;
            MxNode *n = node(N_TRYCATCH);
            expect_punct(p, "{");
            /* parse_body stops AT the closing brace without eating it, so the
             * brace must be consumed here before catchx can be expected. */
            n->a = parse_body(p, "catchx");
            expect_punct(p, "}");
            if (!at_kw(p, "catchx")) mx_raise("Expected catchx at line %d", peek(p)->line);
            p->i++;
            /* catchx is spelled `catchx(e)`, so the '(' comes BEFORE the
             * variable - unlike func f(x), where it follows the name. */
            expect_punct(p, "(");
            Token *id = next(p);
            if (id->type != T_IDENT) mx_raise("Expected catch variable at line %d", id->line);
            n->catch_var = arena_strdup(id->text, strlen(id->text));
            expect_punct(p, ")");
            expect_punct(p, "{");
            n->catch_body = parse_body(p, NULL);
            expect_punct(p, "}");
            n->line = t->line;
            return n;
        }
        if (strcmp(t->text, "classx") == 0) {
            /* classx Name [extends Parent] { let f = v  func m(x) { } }
             *
             * The GRAMMAR keeps `init` in its OWN slot (:134-140) and the FIELD
             * list separate, because AGENTS.md's invariant is that init is NOT
             * among the methods - and find_method is the only lookup path. A
             * parser that appended init to `methods` would pass every simple
             * test and break `super.init`, which is the release that shipped
             * "super has no method 'init'". */
            MxNode *n = node(N_CLASSDEF);
            Token *id;
            p->i++;                       /* consume `classx` */
            id = next(p);                 /* the NAME */
            if (id->type != T_IDENT) mx_raise("Expected class name at line %d", id->line);
            n->name = arena_strdup(id->text, strlen(id->text));
            n->line = id->line;
            if (at_kw(p, "extends")) {
                p->i++;
                {   Token *pn = next(p);
                    if (pn->type != T_IDENT)
                        mx_raise("Expected parent class name at line %d", pn->line);
                    n->parent = arena_strdup(pn->text, strlen(pn->text));
                }
            }
            expect_punct(p, "{");
            {   int fcap = 4, mcap = 4;
                n->nfields = 0; n->nmethods = 0; n->init = NULL;
                n->fields  = (MxNode **)arena_alloc((size_t)fcap * sizeof(MxNode *));
                n->methods = (MxNode **)arena_alloc((size_t)mcap * sizeof(MxNode *));
                while (!at_punct(p, "}")) {
                    if (at_kw(p, "let")) {
                        MxNode *f = node(N_FIELD);
                        Token *fid;
                        p->i++;
                        fid = next(p);
                        if (fid->type != T_IDENT)
                            mx_raise("Expected field name at line %d", fid->line);
                        f->name = arena_strdup(fid->text, strlen(fid->text));
                        expect_punct(p, "=");
                        f->a = parse_expr(p);
                        f->line = fid->line;
                        if (n->nfields >= fcap) {
                            MxNode **nf = (MxNode **)arena_alloc(
                                (size_t)fcap * 2 * sizeof(MxNode *));
                            int bi; for (bi = 0; bi < fcap; bi++) nf[bi] = n->fields[bi];
                            n->fields = nf; fcap = fcap * 2;
                        }
                        n->fields[n->nfields++] = f;
                        continue;
                    }
                    if (at_kw(p, "func")) {
                        MxNode *m;
                        p->i++;                   /* consume `func` */
                        m = node(N_FUNCDEF);
                        {   Token *mid = next(p);
                            if (mid->type != T_IDENT)
                                mx_raise("Expected method name at line %d", mid->line);
                            m->name = arena_strdup(mid->text, strlen(mid->text));
                            m->line = mid->line;
                        }
                        expect_punct(p, "(");
                        {   int pcap = 4;
                            m->nparams = 0;
                            m->params = (const char **)arena_alloc(
                                (size_t)pcap * sizeof(char *));
                            while (peek(p)->type == T_IDENT) {
                                if (m->nparams >= pcap) {
                                    const char **np = (const char **)arena_alloc(
                                        (size_t)pcap * 2 * sizeof(char *));
                                    int bi; for (bi = 0; bi < pcap; bi++) np[bi] = m->params[bi];
                                    m->params = np; pcap = pcap * 2;
                                }
                                m->params[m->nparams++] =
                                    arena_strdup(peek(p)->text, strlen(peek(p)->text));
                                p->i++;
                                if (!at_punct(p, ",")) break;
                                p->i++;
                            }
                        }
                        expect_punct(p, ")");
                        expect_punct(p, "{");
                        {   MxNode *inner = parse_body(p, NULL);
                            m->body = inner->body; m->nbody = inner->nbody;
                        }
                        expect_punct(p, "}");
                        /* `init` goes to its OWN slot. This is the line that
                         * AGENTS.md's invariant exists to protect. */
                        if (strcmp(m->name, "init") == 0 && n->init == NULL) {
                            n->init = m;
                        } else {
                            if (n->nmethods >= mcap) {
                                MxNode **nm = (MxNode **)arena_alloc(
                                    (size_t)mcap * 2 * sizeof(MxNode *));
                                int bi; for (bi = 0; bi < mcap; bi++) nm[bi] = n->methods[bi];
                                n->methods = nm; mcap = mcap * 2;
                            }
                            n->methods[n->nmethods++] = m;
                        }
                        continue;
                    }
                    mx_raise("Unexpected token %s in class body at line %d",
                             peek(p)->text, peek(p)->line);
                }
            }
            expect_punct(p, "}");
            return n;
        }
        if (strcmp(t->text, "importx") == 0) {
            MxNode *n = node(N_IMPORT);
            Token *id;
            p->i++;
            id = next(p);
            if (id->type != T_IDENT)
                mx_raise("Expected module name after importx at line %d", id->line);
            n->name = arena_strdup(id->text, strlen(id->text));
            n->line = id->line;
            return n;
        }
        if (strcmp(t->text, "func") == 0) {
            p->i++;
            MxNode *n = node(N_FUNCDEF);
            Token *id = next(p);
            if (id->type != T_IDENT) mx_raise("Expected function name at line %d", id->line);
            n->name = arena_strdup(id->text, strlen(id->text));
            expect_punct(p, "(");
            /* The parameters' NAMES matter: they are how the body's Idents
             * resolve. A LIST, because arity is arbitrary - case 19 is the first
             * two-argument function in the corpus, and `interp.mx` has 150
             * functions. Bound by POSITION at call time, so the order written here
             * is the order the caller must use. */
            {
                int pcap = 4;
                n->nparams = 0;
                n->params = (const char **)arena_alloc((size_t)pcap * sizeof(char *));
                while (peek(p)->type == T_IDENT) {
                    if (n->nparams >= pcap) {
                        const char **np = (const char **)arena_alloc(
                            (size_t)pcap * 2 * sizeof(char *));
                        int i;
                        for (i = 0; i < pcap; i++) np[i] = n->params[i];
                        n->params = np;
                        pcap = pcap * 2;
                    }
                    n->params[n->nparams++] =
                        arena_strdup(peek(p)->text, strlen(peek(p)->text));
                    p->i++;
                    if (!at_punct(p, ",")) break;
                    p->i++;
                }
            }
            expect_punct(p, ")");
            expect_punct(p, "{");
            {   /* keep the inner Program so the body COUNT is not lost */
                MxNode *inner = parse_body(p, NULL);
                n->body = inner->body;
                n->nbody = inner->nbody;
            }
            expect_punct(p, "}");
            n->line = id->line;
            return n;
        }
        if (strcmp(t->text, "mountx") == 0 || strcmp(t->text, "unmountx") == 0) {
            p->i++;                      /* the wrapper; main.py strips it */
            { MxNode *n = node(N_EXPRSTMT); n->line = t->line; return n; }
        }
    }
    /* assignment: ident '=' expr  (an '=' punct after an identifier) */
    if (t->type == T_IDENT && p->t[p->i + 1].type == T_PUNCT &&
        strcmp(p->t[p->i + 1].text, "=") == 0) {
        MxNode *n = node(N_ASSIGN);
        n->name = arena_strdup(t->text, strlen(t->text));
        p->i += 2;
        n->a = parse_expr(p);
        n->line = t->line;
        return n;
    }
    /* An ASSIGNMENT TARGET that is not a bare name: `a[i] = v` or `this.f = v`.
     * Both are tried BEFORE the ExprStmt fallback and only when the token after
     * the target is "=", so a bare `a[i]` or `obj.f` still parses as an
     * expression statement (parser.py:159-166 does exactly this lookahead).
     *
     * The FIELD form has to be handled here because `this` is a bare identifier:
     * the name form above matches `this` and stops, leaving the `=` unconsumed -
     * which reported "Unexpected token = at line 3" and named a token that was
     * perfectly correct three tokens after the real problem. */
    if ((t->type == T_KW && strcmp(t->text, "this") == 0) ||
        (t->type == T_IDENT)) {
        size_t save2 = p->i;
        MxNode *lhs = parse_stmt_expr_target(p);
        if (lhs && at_punct(p, "=")) {
            MxNode *n = node(N_ASSIGN);
            n->name = NULL;            /* the target is n->b */
            p->i++;
            n->a = parse_expr(p);
            n->b = lhs;
            n->line = t->line;
            return n;
        }
        p->i = save2;
    }
    /* a[expr] = value - an INDEX target. It must be tried BEFORE the ExprStmt
     * fallback, and only when the token after the closing bracket is "=", so
     * that a bare `a[i]` statement still parses as an expression statement. */
    if (t->type == T_IDENT && p->t[p->i + 1].type == T_PUNCT &&
        strcmp(p->t[p->i + 1].text, "[") == 0) {
        size_t save = p->i;
        MxNode *lhs = parse_postfix(p, parse_primary(p));
        if (at_punct(p, "=")) {
            MxNode *n = node(N_ASSIGN);
            n->name = NULL;            /* the target is n->b, an N_INDEX */
            p->i++;
            n->a = parse_expr(p);
            n->b = lhs;
            n->line = t->line;
            return n;
        }
        p->i = save;                  /* not an assignment after all */
    }
    {   MxNode *n = node(N_EXPRSTMT); n->a = parse_expr(p); n->line = t->line; return n; }
}
/* ===================================================================== */
/* 8. Evaluator                                                          */
/* read_file and parse live further down (sections 7 and main). The import walk
 * needs them and they need nothing from it, so forward declarations are honest
 * here; moving either would reorder the file for a dependency that is genuinely
 * one-way. */
static char   *read_file(const char *path);

/* parse - the token-stream to AST entry point, SKIPPING the mountx/unmountx
 * wrapper (which main() used to do inline, in two places' worth of duplication
 * the moment an import needed it too). Declared here because the import walk is
 * ABOVE its definition; a module that silently lost its `mountx` would be parsed
 * as a bare body and fail on its first import - a confusing message for a
 * module that is perfectly well formed. */
static MxNode *parse(P *p);

/* mx_exec_import is defined below mx_load_module (which calls it for a nested
 * importx) but the N_IMPORT statement arm calls it too - so both directions
 * exist and a forward declaration is the honest way to say so. */
static void    mx_exec_import(MxNode *s, MxEnv *env);

const char *g_script_dir = NULL;

/* g_module_env - the private scope of the module currently LOADING. Its lets
 * are evaluated in it, and a module function's body resolves bare references
 * through it (:243-247). A stack of one is enough because module loading does
 * not recurse into itself except through importx, which is handled by the
 * `saved` in mx_load_module. */
static MxEnv *g_module_env = NULL;

/* mx_func_value - a function AST node as a VALUE. MX_FUNC carries it in the
 * box union, and display renders it as <func> - the host's `<func NAME>` shape
 * (:84) needs the name, which mx_display_into has, so this is the minimal
 * faithful box and not a second representation. */
static MxValue mx_func_value(MxNode *fn) {
    MxValue v;
    v.tag = MX_FUNC;
    v.as.box = fn;
    return v;
}

/* mx_cwd - the cwd, third base (:201). */
static const char *mx_cwd(void) {
    static char buf[1024];
    if (!mx_os_getcwd(buf, sizeof buf)) snprintf(buf, sizeof buf, ".");
    return buf;
}

/* mx_first_is_mountx - does this token stream start with `mountx`? */
static int mx_first_is_mountx(Token *toks) {
    return toks && toks[0].type == T_KW && strcmp(toks[0].text, "mountx") == 0;
}

/* mx_load_module - parse and RUN a module in its own private scope.
 *
 * The module's top level runs with `env` SWAPPED to a private scope whose parent
 * is globals (:243-247). That is what makes a module's `let` invisible to its
 * importer while its functions still work: each call chains THIS scope under
 * itself, so a bare module-let reference resolves (the host's __mod_env__).
 *
 * The three export slots are separate dicts - funcs, classes, lets - because
 * eval_dot reads them IN THAT ORDER (:599-605). A single merged table would make
 * the answer depend on insertion order instead of on which kind of thing the
 * name denotes.
 *
 * A module OBJECT is bound into globals ONLY if the name is not already taken by
 * a non-module (:258-260) - the host's protection for module objects, which
 * AGENTS.md records as the "func map shadowed the map module" fix. */
static void mx_load_module_code(const char *code, const char *name, MxEnv *env) {
    /* `env` - the IMPORTER's scope - is deliberately NOT this module's parent.
     * The module gets a fresh scope under GLOBALS (:243-247), which is why a
     * module's let is invisible to its importer. Discarded explicitly: an unused
     * parameter is a -Wall warning, and G3's target is zero. */
    (void)env;
    Token *toks = lex(code);
    P p;
    MxNode *ast;
    MxEnv *mod_scope;
    MxDict *funcs = mx_dict_new(8);
    MxDict *classes = mx_dict_new(8);
    MxDict *lets = mx_dict_new(8);
    MxModule *mod;
    MxValue mv;
    int i;

    /* The auto-wrap main.py does (:31-32): a module file need not carry its own
     * mountx header, and interp.mx relies on that. The test is on the first
     * token's TEXT rather than on a MOUNTX token type, because the lexer has no
     * such type - "mountx" is an ordinary keyword here. Getting that name wrong
     * is not a compile error, it is a double-wrapped module that then fails to
     * parse on an inner `unmountx`. */
    if (!toks || !mx_first_is_mountx(toks)) {
        size_t need = strlen(code) + 24;
        char *wrapped = (char *)arena_alloc(need);
        snprintf(wrapped, need, "mountx\n%s\nunmountx\n", code);
        p.t = lex(wrapped);
    } else {
        p.t = toks;
    }
    p.i = 0;
    ast = parse(&p);

    mod_scope = env_new(g_globals);
    /* every module function and method CHAINS this scope under its calls, so a
     * bare module let resolves - the __mod_env__ stamp, applied as a runtime
     * property rather than resolved at compile time (COMPILE_DESIGN 8.1). */
    {   MxEnv *saved = g_module_env;
        int j;
        g_module_env = mod_scope;
        for (i = 0; i < ast->nbody; i++) {
            MxNode *st = ast->body[i];
            if (st->tag == N_FUNCDEF) {
                st->mod_env = mod_scope;   /* the __mod_env__ stamp */
                mx_dict_set(funcs, st->name, mx_func_value(st));
                /* A module function is ALSO defined in GLOBALS, and that is the
                 * host's rule (interpreter.py:258-261), not a convenience.
                 *
                 * Two things depend on it. One is a module-level `let` that CALLS
                 * a function of its own module - lexer.mx:185 is
                 * `let _WORDS = _build_words()`, and without the global binding
                 * the call has nowhere to resolve and the compiled interpreter
                 * died on "'_build_words' is not callable" while loading the
                 * lexer. The other is a bare call from anywhere, which is what
                 * AGENTS.md means by "module function names are global".
                 *
                 * The guard is the host's too: a MODULE OBJECT is never
                 * shadowed, so a module function whose name collides with an
                 * imported module's name is not defined. */
                {   MxValue existing;
                    if (env_get(g_globals, st->name, &existing) &&
                        existing.tag == MX_MOD)
                        ;   /* a module object wins: never shadowed */
                    else
                        env_define(g_globals, st->name, mx_func_value(st));
                }
            } else if (st->tag == N_CLASSDEF) {
                mx_define_class(st->name, st);
                for (j = 0; j < st->nmethods; j++)
                    st->methods[j]->mod_env2 = mod_scope;
                if (st->init) st->init->mod_env2 = mod_scope;
                { MxValue cv; cv.tag = MX_FUNC; cv.as.box = st;
                  mx_dict_set(classes, st->name, cv); }
            } else if (st->tag == N_LET) {
                /* A module let is BOTH exported and DEFINED IN THE MODULE'S
                 * SCOPE. Exporting alone puts it in the `lets` dict, which the
                 * importer reads through `mod.SHARED` - but a module FUNCTION
                 * reading a bare `SHARED` looks it up in its frame, which chains
                 * under mod_scope. With the value only in the dict, that read
                 * reported "Undefined variable 'SHARED'" - and the __mod_env__
                 * stamp that exists precisely to prevent that was doing nothing.
                 *
                 * The host gets both from one execution: its `let` statement runs
                 * with self.env = mod_scope (:245-246), so the binding lands in
                 * the scope AND the module object records it as an export. Two
                 * steps here, because the module object is a separate box. */
                {   MxValue lv = mx_eval(st->a, mod_scope);
                    env_define(mod_scope, st->name, lv);
                    mx_dict_set(lets, st->name, lv);
                }
            } else if (st->tag == N_IMPORT) {
                mx_exec_import(st, mod_scope);
            }
        }
        g_module_env = saved;
    }

    mod = (MxModule *)arena_alloc(sizeof(MxModule));
    mod->stamp = mx_next_stamp();
    mod->mod_name = arena_strdup(name, strlen(name));
    mod->funcs = funcs;
    mod->classes = classes;
    mod->lets = lets;
    mv.tag = MX_MOD; mv.as.box = mod;
    {   MxValue existing;
        if (env_get(g_globals, name, &existing) && existing.tag == MX_MOD) {
            return;   /* module objects are NEVER shadowed (AGENTS.md) */
        }
    }
    env_define(g_globals, name, mv);
}

static void mx_load_module(const char *path, const char *name, MxEnv *env) {
    char *code = read_file(path);
    mx_load_module_code(code, name, env);
}

/* mx_eval_dot_ast / mx_ast_set_field - the AST half's DotAccess and its field
 * store, made VISIBLE to the generated path.
 *
 * They are wrappers rather than the real functions because the real ones are
 * static to the AST half and its write path lives inside mx_eval's DotAccess
 * arm. mx_ast_set_field is therefore a genuine re-implementation of the STORE -
 * the AST half has no standalone writer - and it is small on purpose: a module
 * instance's field is a plain dict entry, which is what the AST half's own
 * instance arm does first. Anything richer (a class-chain declared field with no
 * dict entry) is not reachable from a module instance, because mx_load_module
 * MERGES the declared fields into the dict when it builds the instance. */
#ifdef MX_BOTH
static MxValue mx_eval_dot_ast(MxValue obj, const char *field) {
    return mx_eval_dot(obj, field);
}
static void mx_ast_set_field(MxValue obj, const char *field, MxValue val) {
    MxInstance *inst;
    if (obj.tag != MX_INST)
        mx_raise("Cannot set field '%s' on %s", field, mx_type_name(obj));
    inst = (MxInstance *)obj.as.box;
    if (!inst->fields) inst->fields = mx_dict_new(8);
    mx_dict_set(inst->fields, field, val);
}

/* mx_ast_call_method - call a method on an AST instance.
 *
 * It finds the method with the AST half's OWN mx_find_method over the class
 * chain, then calls it with the receiver bound through the AST half's
 * mx_call_instance. Both steps are the host's (eval_call's BoundMethod arm), and
 * reusing them is the point: a module class's method must see `this`, must see
 * its own module's lets through __mod_env__, and must report the host's arity
 * error - all of which those two functions already do. A generated method call
 * would see none of it. */
static MxValue mx_ast_call_method(MxValue recv, const char *name,
                                  MxValue *args, int nargs) {
    MxInstance *inst = (MxInstance *)recv.as.box;
    MxNode *chain[MX_CHAIN_MAX];
    int nchain = mx_class_chain(inst->klass, chain), i;
    for (i = 0; i < nchain; i++) {
        MxNode *m = mx_find_method(chain[i], name);
        if (m) return mx_call_instance(m, args, nargs, inst, chain[i]);
    }
    mx_raise("Field/method '%s' not found on %s", name,
             inst->klass ? inst->klass->name : "?");
    return mx_null();
}
#endif

/* ---- mx_exec_import - the FOUR-BASE walk (interpreter.py:186-222) --------
 *
 * D-1's SHAPE SURVIVES INTO C, and that is the point of measuring it. The
 * compiled binary's search list names ITS OWN directory, not the Python
 * interpreter's: the last base is the directory holding `mini.c` - found at RUN
 * time from argv[0] or MOUNTX_BIN_DIR, never a compile-time path. A compiled
 * interpreter therefore resolves imports relative to the binary the user ran,
 * which is the same rule the source form follows relative to interpreter.py.
 * A version that baked the build machine's paths in would work HERE and fail on
 * every other machine - the most expensive possible way to be wrong.
 *
 * Per base, the candidates are stdlib/<sub>/, stdlib/<>, libs/<sub>/, libs/<>,
 * then <sub>/ and <> (:203-211), and the SUBS list is ["", core, data,
 * structs, text, math, system] - flat stdlib FIRST. Duplicates are dropped and
 * order is preserved, and the ERROR names the whole search list (:222) because a
 * module-not-found message that says only "not found" makes the user guess. */
/* g_argv0 - the running binary's path, captured in main. The LAST import base is
 * this binary's directory (:201), and reading it at RUN time is what keeps D-1's
 * shape honest in C. */
/* g_imported - the set of module names already loaded (:186-187), so a module
 * imported twice runs ONCE. It is checked BEFORE the walk, and it is a dict of
 * booleans rather than a flag, because the host's `_imported` is a set and two
 * modules may legitimately import the same third module. */
/* SELF-INITIALISING, through mx_imported_set().
 *
 * It used to be allocated in main, which was correct for the AST half (main is
 * the only entry point) and wrong for a generated program - whose main is emitted
 * and knows nothing about this set. The first B-2 import segfaulted on
 * `mx_dict_has(NULL, ...)`. Two ways to fix: teach the emitter about the set, or
 * make the set impossible to forget. The second is right - an allocator here is
 * the same lazy-initialiser pattern the arena already uses, and it removes a
 * piece of entry-point bookkeeping rather than adding one. */
static MxDict *g_imported = NULL;

static MxDict *mx_imported_set(void) {
    if (!g_imported) g_imported = mx_dict_new(16);
    return g_imported;
}

/* g_script_dir - the ENTRY SCRIPT's directory, the second search base (:200).
 * Empty for a REPL or `-e`, which is why those two resolve from the cwd. */
static int mx_file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *g_search_log = NULL;   /* the list, for the not-found text */
static size_t g_search_len = 0;

/* search_add - append one candidate path to the not-found log.
 *
 * The buffer TRACKS ITS OWN CAPACITY. The first version compared
 * `g_search_len + need > 1024` against a hardcoded 1024 while growing the buffer
 * to `g_search_len + need + 1024`, so after one growth the test compared against
 * a number that no longer described the allocation - and since the new size is
 * always larger than the test, the loop never exited and `importx` of a
 * NON-EXISTENT module hung forever instead of reporting the search list. The
 * found-module path never grew the buffer, which is why only the not-found path
 * hung; and the message the hang was supposed to produce is the one that tells a
 * user where the interpreter looked. */
static size_t g_search_cap = 0;

static void search_add(const char *path) {
    size_t need = strlen(path) + 4;
    char *n;
    if (!g_search_log) {
        g_search_cap = 4096;
        g_search_log = (char *)arena_alloc(g_search_cap);
        g_search_len = 0;
        g_search_log[0] = '\0';
    }
    if (g_search_len + need > g_search_cap) {
        size_t ncap = (g_search_len + need) * 2;
        char *bigger = (char *)arena_alloc(ncap);
        memcpy(bigger, g_search_log, g_search_len + 1);
        g_search_log = bigger;
        g_search_cap = ncap;
    }
    n = g_search_log + g_search_len;
    /* Python's repr puts the CLOSING quote then the separator: "a', 'b". My
     * first separator was the other way round, so every path after the first
     * carried its opening quote and the previous path's closing one was
     * missing - the same text, with the punctuation transposed. */
    if (g_search_len) { memcpy(n, "', '", 4); n += 4; g_search_len += 4; }
    memcpy(n, path, strlen(path) + 1);
    g_search_len += strlen(path);
}

/* mx_same_dir - do two paths name the same directory? Compared after stripping a
 * trailing slash and after making an ABSOLUTE path, because the host's last base
 * is `os.path.dirname(os.path.abspath(__file__))` - absolute - while a compiled
 * binary's equivalent is derived from argv[0] and can be relative. Comparing the
 * strings would keep both, and the not-found list would show the same directory
 * twice under two spellings. */
/* MX_PATH_MAX is the longest string this file builds: a cwd (1024) plus a
 * separator plus a relative base (1024), so a plain 1024 buffer for the JOINED
 * path is what -Wformat-truncation is complaining about. The joined path needs
 * 2049, not 1024 - and truncating it here would compare two DIFFERENT prefixes
 * and call two identical directories different. */
#define MX_PATH_MAX 2049

static int mx_is_abs_path(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
#ifdef _WIN32
    if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') return 1;
#endif
    return 0;
}

static int mx_same_dir(const char *a, const char *b) {
    char ra[MX_PATH_MAX], rb[MX_PATH_MAX];
    if (!mx_is_abs_path(a)) {
        char cwd[1024];
        if (!mx_os_getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        snprintf(ra, sizeof ra, "%s/%s", cwd, a);
    } else {
        snprintf(ra, sizeof ra, "%s", a);
    }
    snprintf(rb, sizeof rb, "%s", b);
    {   size_t la = strlen(ra), lb = strlen(rb);
        if (la > 1 && ra[la - 1] == '/') ra[la - 1] = '\0';
        if (lb > 1 && rb[lb - 1] == '/') rb[lb - 1] = '\0';
    }
    return strcmp(ra, rb) == 0;
}

static void search_add_unique(const char *path) {
    /* duplicates are dropped by checking the text already logged - the list is
     * short (4 bases x 21 candidates) so a scan beats a hash set, and it keeps
     * the ORDER the host prints. */
    const char *p = g_search_log;
    size_t plen = strlen(path);
    while (p && *p) {
        const char *q = p;
        size_t l = 0;
        while (q[l] && q[l] != ' ') l++;
        if (l == plen && strncmp(q, path, plen) == 0) return;
        p = q[l] ? q + l + 1 : NULL;
    }
    search_add(path);
}

/* mx_exec_import - run the walk, then EXECUTE the module in its own scope.
 *
 * The module's top level runs in a PRIVATE scope whose parent is globals
 * (:243-247), not in globals - so a module's `let` is not visible to its
 * importer, while its functions and methods keep working because each call
 * chains that private scope under itself. That is `__mod_env__` in embryo, and
 * it is why a module's function can read a bare module let.
 *
 * The importer receives a MODULE OBJECT, never the module's functions one by one:
 * funcs/classes/lets are three separate slots because eval_dot reads them in
 * that order (:599-605), and merging them would change which one wins. */
static void mx_exec_import(MxNode *s, MxEnv *env) {
    static const char *SUBS[] = { "", "core", "data", "structs", "text",
                                  "math", "system", NULL };
    const char *name = s->name;
    const char *bases[4];
    int nb = 0, bi, si, ri;
    char found[1024];
    int got = 0;
    const char *env_std = getenv("MOUNTX_STD");

    {   MxDict *seen = mx_imported_set();
        if (mx_dict_has(seen, name)) return;
        mx_dict_set(seen, name, mx_bool(1));
    }

    {
        const char *aot_src = mx_aot_get_source(name);
        if (aot_src) {
            mx_load_module_code(aot_src, name, env);
            return;
        }
    }

    if (env_std && *env_std) bases[nb++] = env_std;
    if (g_script_dir && *g_script_dir && !mx_same_dir(g_script_dir, env_std))
        bases[nb++] = g_script_dir;
    /* bases is DEDUPED and order-preserved (:213-217). Two bases can be the same
     * directory - a script run from the repo root with MOUNTX_STD pointing at
     * ./stdlib would otherwise search the cwd twice - and the host drops the
     * duplicate so the not-found list does not repeat itself. */
    {   const char *cwd = mx_cwd();
        int dup = 0, i;
        for (i = 0; i < nb; i++) if (mx_same_dir(bases[i], cwd)) { dup = 1; break; }
        if (!dup) bases[nb++] = cwd;
        dup = 0;
        for (i = 0; i < nb; i++) if (mx_same_dir(bases[i], mx_bin_dir())) { dup = 1; break; }
        /* The bin-dir base is D-1's shape and it is NOT a divergence to keep
         * silent: the host's fourth base is interpreter.py's own directory, and
         * a compiled binary's equivalent is the BINARY's directory. On this
         * machine they differ (interpreter.py sits at the repo root; the binary
         * in selfhost/b1), so the not-found list legitimately names one more
         * base than the source form. That is the shape surviving, not a bug -
         * and it is exactly why the not-found message is part of a corpus case
         * rather than a comment. */
        if (!dup) bases[nb++] = mx_bin_dir();
    }

    g_search_log = NULL; g_search_len = 0;
    for (bi = 0; bi < nb && !got; bi++) {
        for (ri = 0; ri < 3 && !got; ri++) {
            const char *root = (ri == 0) ? "stdlib" : (ri == 1) ? "libs" : NULL;
            for (si = 0; SUBS[si] && !got; si++) {
                char cand[1024 + 32];   /* base + longest suffix */
                if (root) {
                    if (SUBS[si][0]) snprintf(cand, sizeof cand, "%s/%s/%s/%s.mx",
                                               bases[bi], root, SUBS[si], name);
                    else           snprintf(cand, sizeof cand, "%s/%s/%s.mx",
                                               bases[bi], root, name);
                } else {
                    if (SUBS[si][0]) snprintf(cand, sizeof cand, "%s/%s/%s.mx",
                                               bases[bi], SUBS[si], name);
                    else           snprintf(cand, sizeof cand, "%s/%s.mx",
                                               bases[bi], name);
                }
                search_add_unique(cand);
                if (mx_file_exists(cand)) {
                    snprintf(found, sizeof found, "%s", cand);
                    got = 1;
                }
            }
        }
    }
    if (!got) {
        /* The host interpolates a PYTHON LIST (:222), so the frozen text carries
         * Python's quoting: ['a/b.mx', 'c/d.mx']. Emitting a bare space-separated
         * run would be a divergence in a message whose whole purpose is telling
         * the user where the interpreter looked. */
        mx_raise("Module '%s' not found. Searched: ['%s']", name,
                 g_search_log ? g_search_log : "");
        return;
    }
    mx_load_module(found, name, env);
}

/* ===================================================================== */


static MxValue mx_eval(MxNode *n, MxEnv *env);

/* --- binop: the ARM ORDER is the host's, not a convenient one ----------
 * interpreter.py:550-556 tests string FIRST, so `1 + "a"` concatenates and the
 * numeric arm is never reached. Corpus case 04 pins this; my first draft got it
 * wrong and the corpus caught it. */

/* --- the call frame. Its env's parent is GLOBALS, never the caller's env:
 * that is the whole lexical-scope story, and corpus case 09 pins it. */

static MxNode *g_funcs[32];
static int g_nfuncs = 0;

static MxNode *find_func(const char *name) {
    if (!name) return NULL;
    const char *iname = mx_fast_intern(name);
    int i;
    for (i = 0; i < g_nfuncs; i++)
        if (g_funcs[i]->name == iname) return g_funcs[i];
    return NULL;
}

static void mx_exec_block(MxNode **body, int n, MxEnv *env);
/* mx_call_fn_list now executes statements DIRECTLY (so it can see a pending
 * return between them), so mx_exec_stmt must be visible above it rather than
 * only where it is defined. */
static void mx_exec_stmt(MxNode *s, MxEnv *env);

/* mx_call_fn_list - the ONE calling convention.
 *
 * The single-argument mx_call_fn that preceded it is GONE, not kept alongside:
 * once this existed nothing called the wrapper, and a function with no caller is
 * exactly what G3's -Wunused-function reports. Two copies of the binding and
 * RETURN logic would also have been two places for those semantics to disagree.
 */
/* mx_call_instance - a METHOD call: bind `this`, bind the declared parameters,
 * and save/restore the receiver so a nested call does not steal it.
 *
 * The receiver is saved and RESTORED rather than cleared, so a method that calls
 * another method and then reads `this` or `super` still sees its own receiver -
 * not the callee's. That is what "the receiver comes from the call frame" means
 * once calls nest, and clearing instead of restoring is the bug that makes a
 * subclass quietly operate on its parent.
 *
 * Parameters are bound BY POSITION from the declaration, so a caller's argument
 * order is the callee's parameter order - there is no name matching anywhere,
 * which is why a typo in a parameter name is a compile error rather than a
 * silent unbound variable. */
static MxValue mx_call_instance(MxNode *fn, MxValue *argv, int nargs,
                                MxInstance *inst, MxNode *defining) {
    /* The parent is the __mod_env__ STAMP when the method has one - the same
     * rule as mx_call_fn_list, on the method's own stamp field (mod_env2,
     * written by handle_import at 4515-4516). A method defined in an imported
     * module reads that module's private lets (`_SYM1` in lexer.mx,
     * `_NONE` in parser.mx) through this chain; parenting straight to
     * g_globals made every bare module-let reference inside a method raise
     * "Undefined variable '_SYM1'" - which, before the swallow fix, was the
     * error that never surfaced at all. A method of a guest class has no
     * stamp, and its module IS globals (interpreter.py:879). */
    MxEnv *fenv = env_new(fn->mod_env2 ? (MxEnv *)fn->mod_env2 : g_globals);
    /* volatile on EVERY local that is read after the longjmp, not just the
     * result. C99 7.13.2.1p3: a non-volatile automatic whose value changed
     * between setjmp and longjmp is INDETERMINATE. The receiver saves are
     * restored after the catch, so without volatile a method could restore the
     * callee's stale receiver and a subclass would quietly operate on its
     * parent - which is exactly the failure this function's comment warns about.
     * -Wclobbered names all three, and it is right about each. */
    volatile MxValue saved_receiver = g_receiver;
    volatile int saved_have = g_have_receiver;
    /* `const`, not `volatile`: what must not change here is the POINTER, and
     * `volatile` on a pointer also qualifies the TARGET, which then has to be
     * discarded at every read - three warnings for one idea. `const` states the
     * same intent (this value does not change) with no contamination. */
    const MxNode *saved_class = g_receiver_class;
    volatile MxValue r = mx_null();
    int i;
    volatile int hit;

    {   MxValue rv; rv.tag = MX_INST; rv.as.box = inst;
        g_receiver = rv;
        g_have_receiver = 1;
        g_receiver_class = defining;
    }
    /* `this` is an IDENT named "this" in the AST, resolved here rather than from
     * the environment - so a program cannot rebind it. */
    {   MxValue rv; rv.tag = MX_INST; rv.as.box = inst;
        env_define(fenv, "this", rv);
    }
    for (i = 0; i < fn->nparams && i < nargs; i++) {
        env_define(fenv, fn->params[i], argv[i]);
    }

    /* The body runs inside a pushed catch frame, so a `return` from ANY depth -
     * out of a loop, an if, a tryx - lands HERE. Every frame below declines
     * K_RET and re-throws it upward (B-3.2's stack). */
    hit = mx_try(mx_exec_block(fn->body, fn->nbody, fenv));
    /* THE METHOD BOUNDARY DECLINES K_THROW / K_ERROR, exactly as
     * mx_call_fn_list does - a signal absorbed by a frame that does not own it
     * is a wrong ANSWER (AGENTS.md's rule, and fn_list's comment says the same).
     * This site used to be `if (hit) r = g_sig.value; mx_clear_signal();`,
     * which swallowed EVERY signal raised inside a method: an error returned
     * null and execution CONTINUED after the call as if nothing happened.
     * Two visible defects came from it: the ported lexer's err() never unwound,
     * so its position never advanced and the token loop spun until the arena
     * hit 2048 MB (the punctuation OOM - a single `=` as a whole program); and
     * an unsupported index target inside a method stored first, raised second,
     * and the raise was eaten - so the store stuck but every STATEMENT AFTER IT
     * was skipped (hashtable size() == 0 with the entry present).
     * The receiver save is restored BEFORE the rethrow because the longjmp
     * skips the epilogue below - same volatile discipline as the K_RET path. */
    if (hit && mx_caught_kind() != K_RET) {
        {   MxValue rv = saved_receiver;
            int     rh = saved_have;
            MxNode *rc = (MxNode *)saved_class;
            g_receiver = rv;
            g_have_receiver = rh;
            g_receiver_class = rc;
        }
        env_free(fenv);
        mx_rethrow();                 /* the CALLER's catcher owns it */
        return mx_null();             /* unreachable: renders and exits */
    }
    if (hit) r = g_sig.value;
    mx_clear_signal();

    /* The SAVED locals are volatile because they are read after a longjmp; the
     * globals they restore are not. Reading a volatile pointer into a plain one
     * is a discard - which -Wall reports - so the copy goes through a temporary
     * rather than through a cast, because a cast would silence a real warning
     * about a real qualifier difference. */
    /* The globals are plain; the SAVES are volatile because they are what
     * survives the longjmp. Restoring reads a volatile value into a plain
     * global, which is the safe direction - the qualifier is on the SOURCE of
     * the read, and nothing downstream needs to know. Casting would be worse:
     * it would hide the fact that this state is longjmp-sensitive, which is the
     * whole reason the saves are volatile. */
    /* Reading a volatile POINTER into a plain one is a qualifier discard, which
     * -Wall reports - so the read goes through an explicitly-typed temporary.
     * A cast would silence the warning while hiding the reason these saves are
     * volatile at all: they are read AFTER a longjmp. */
    {   MxValue rv = saved_receiver;
        int     rh = saved_have;
        MxNode *rc = (MxNode *)saved_class;
        MxValue rr = r;
        g_receiver = rv;
        g_have_receiver = rh;
        g_receiver_class = rc;
        env_free(fenv);
        return rr;
    }
}

static MxValue mx_call_fn_list(MxNode *fn, MxValue *args, int nargs, MxEnv *globals) {
    /* The parent is the __mod_env__ STAMP when the function has one, and globals
     * otherwise (interpreter.py:879: `parent = func_def.get("__mod_env__") or
     * self.globals`).
     *
     * This is COMPILE_DESIGN 8.1's whole point, and it is the reason a compiler
     * must NOT resolve the parent at emit time: the stamp is written by
     * handle_import AFTER parsing, and whether a function has one depends on HOW
     * it was imported - a runtime fact. Emitting `globals` here would pass
     * fbtest (whose module functions do not read module lets) and break every
     * module-let case, which is the I-M4 defect class AGENTS.md records once
     * already. A MODULE function therefore sees its own lets; a plain function
     * sees globals. */
    MxEnv *call_env = env_new(fn->mod_env ? (MxEnv *)fn->mod_env : globals);
    int i;
    /* Bound BY POSITION, so params[0] is the first argument. A parameter with no
     * argument is left UNDEFINED rather than bound to null: `mx_get` then reports
     * "Undefined variable 'x'", which names the missing name, whereas binding
     * null would report a type error somewhere downstream. (The host is looser
     * here - it binds whatever it has - but the corpus never under-supplies, and
     * the louder failure is the better one.) */
    for (i = 0; i < fn->nparams; i++) {
        if (i < nargs) env_define(call_env, fn->params[i], args[i]);
    }
    {   int k;
        /* A function body starts with NO pending loop signal: a break cannot
         * cross a call boundary, which is why g_loop_sig must be cleared here
         * (the host makes this a SyntaxError instead). */
        mx_clear_loop_signal();
        /* The function BOUNDARY is a catcher like any other: it pushes a frame so
         * a `return` from any depth lands HERE. Every loop and tryx between the
         * return and this frame declines the signal and re-throws it upward, so
         * this is the frame that finally takes it - and it is guaranteed to
         * exist, which is why a return can never fall off the end. */
        /* volatile because it SURVIVES a longjmp. The C standard says a
         * non-volatile automatic whose value changed between setjmp and longjmp
         * is indeterminate, and -Wclobbered - which this project compiles with on
         * purpose - names exactly this variable. The warning is not noise to be
         * silenced; it is the compiler agreeing that the catch discipline has to
         * be declared, not assumed. */
        const char *prev_func = mx_get_current_func();
        const char *prev_file = mx_get_current_file();
        int call_line = mx_get_current_line();
        mx_push_call_frame(prev_func, fn->name, prev_file, call_line);
        volatile MxValue r;
        volatile int hit = mx_try(({
            for (k = 0; k < fn->nbody; k++) mx_exec_stmt(fn->body[k], call_env);
        }));
        mx_pop_call_frame(prev_func);
        mx_set_current_file(prev_file);
        if (hit) {
            int kind = mx_caught_kind();
            /* K_RET is OURS - this is the boundary the return was aimed at. */
            if (kind == K_RET) {
                r = g_sig.value;
                mx_clear_signal();
                env_free(call_env);
                return r;
            }
            /* K_THROW / K_ERROR are NOT ours: the function is not a catcher for
             * them, the CALLER's tryx is. This frame has already been popped by
             * mx_try's catch arm, so mx_rethrow lands on the caller's catcher -
             * which is why a throw from inside a function inside a while reaches
             * the tryx around the CALL.
             *
             * Returning null instead printed "null" where the host printed
             * "caught neg 1": the throw was consumed by the function boundary and
             * became an ordinary null value. A signal absorbed by something that
             * does not own it is a wrong ANSWER, which is the failure mode this
             * whole increment kept running into. */
            env_free(call_env);
            mx_rethrow();
            return mx_null();
        }
        env_free(call_env);
        r = mx_null();
        return r;
    }
}



static inline int mx_is_native_or_builtin_name(const char *name) {
    if (!name || !name[0]) return 0;
    switch (name[0]) {
        case 'a': return (strcmp(name, "appendx") == 0 || strcmp(name, "aot_get_prog") == 0);
        case 'd': return (strncmp(name, "db_", 3) == 0);
        case 'e': return strcmp(name, "exportx") == 0;
        case 'h': return (strcmp(name, "http_get") == 0 || strcmp(name, "http_post") == 0);
        case 'i': return (strcmp(name, "idiv") == 0 || strcmp(name, "inputx") == 0);
        case 'l': return strcmp(name, "len") == 0;
        case 'm': return (strcmp(name, "math_floor") == 0 || strcmp(name, "mx_version") == 0);
        case 'o': return (strncmp(name, "os_", 3) == 0);
        case 'r': return strcmp(name, "readx") == 0;
        case 's': return (strncmp(name, "sys_", 4) == 0 || strcmp(name, "sleep_ms") == 0 || strncmp(name, "str_", 4) == 0);
        case 't': return (strcmp(name, "time_now_ms") == 0 || strcmp(name, "typeofx") == 0 || strncmp(name, "tcp_", 4) == 0);
        case 'w': return strcmp(name, "writex") == 0;
        default: return 0;
    }
}

static MxValue mx_eval(MxNode *n, MxEnv *env) {
    if (n && n->line > 0) g_current_line = n->line;
    switch (n->tag) {
        case N_LITERAL: return n->lit;
        case N_IDENT: {
            MxValue v;
            if (!env_get(env, n->name, &v))
                mx_raise("Undefined variable '%s'", n->name);
            return v;
        }
        case N_BINOP: {
            /* The AST's operator NAME -> the substrate's SYMBOL. parser.py:262
             * stores the token's semantic name, and mx_binop has always been
             * keyed by symbol (B-1's corpus predates the name). One table, in
             * one place: mapping it in the lexer would couple tokenising to
             * evaluation, and mapping it inside mx_binop would need it to accept
             * both spellings forever. */
            const char *sym = n->op;
            if (n->op && n->op[0]) {
                switch (n->op[0]) {
                    case 'P': sym = "+"; break;
                    case 'M': sym = (n->op[1] == 'I') ? "-" : (n->op[1] == 'U' ? "*" : "%"); break;
                    case 'D': sym = "/"; break;
                    case 'E': sym = "=="; break;
                    case 'N': sym = "!="; break;
                    case 'G': sym = (n->op[2] == 'E') ? ">=" : ">"; break;
                    case 'L': sym = (n->op[2] == 'E') ? "<=" : "<"; break;
                    case 'A': sym = "&&"; break;
                    case 'O': sym = "||"; break;
                    default: break;
                }
            }
            if (sym[0] == '&' && sym[1] == '&') {
                if (!mx_fast_truthy(mx_eval(n->a, env))) return mx_bool(0);
                return mx_bool(mx_fast_truthy(mx_eval(n->b, env)));
            }
            if (sym[0] == '|' && sym[1] == '|') {
                if (mx_fast_truthy(mx_eval(n->a, env))) return mx_bool(1);
                return mx_bool(mx_fast_truthy(mx_eval(n->b, env)));
            }
            {
                MxValue va = mx_eval(n->a, env);
                MxValue vb = mx_eval(n->b, env);
                if (va.tag == MX_INT && vb.tag == MX_INT) {
                    if (sym[0] == '+' && sym[1] == '\0') return mx_int(add_i64(va.as.i, vb.as.i));
                    if (sym[0] == '-' && sym[1] == '\0') return mx_int(sub_i64(va.as.i, vb.as.i));
                    if (sym[0] == '*' && sym[1] == '\0') return mx_int(mul_i64(va.as.i, vb.as.i));
                    if (sym[0] == '<') {
                        if (sym[1] == '\0') return mx_bool(va.as.i < vb.as.i);
                        if (sym[1] == '=')  return mx_bool(va.as.i <= vb.as.i);
                    }
                    if (sym[0] == '>') {
                        if (sym[1] == '\0') return mx_bool(va.as.i > vb.as.i);
                        if (sym[1] == '=')  return mx_bool(va.as.i >= vb.as.i);
                    }
                    if (sym[0] == '=' && sym[1] == '=') return mx_bool(va.as.i == vb.as.i);
                    if (sym[0] == '!' && sym[1] == '=') return mx_bool(va.as.i != vb.as.i);
                    if (sym[0] == '%' && sym[1] == '\0' && vb.as.i != 0) return mx_int(va.as.i % vb.as.i);
                }
                return mx_binop(sym, va, vb);
            }
        }
        case N_TYPEOF: return mx_str(mx_type_name(mx_eval(n->a, env)));
        case N_UNARY: return mx_unary(n->op, mx_eval(n->a, env));
        case N_DOT: {
            return mx_eval_dot(mx_eval(n->a, env), n->field);
        }
        case N_SUPER: {
            MxInstance *inst = g_have_receiver
                ? (MxInstance *)g_receiver.as.box : NULL;
            MxNode *defining = g_receiver_class;
            if (!inst || !defining) {
                mx_raise("super used outside a method");
                return mx_null();
            }
            {   MxSuper *sp = (MxSuper *)arena_alloc(sizeof(MxSuper));
                MxValue v;
                sp->instance = inst;
                sp->klass = defining->parent
                    ? mx_resolve_class(defining->parent) : NULL;
                if (!sp->klass && defining->parent) {
                    mx_class_chain(defining, chain_scratch);
                    return mx_null();
                }
                v.tag = MX_SUPER; v.as.box = sp;
                return v;
            }
        }
        case N_NEW: return mx_eval_new(n, env);
        case N_ARRAYLIT: {
            int i;
            MxValue out = mx_arr_value(mx_arr_new((size_t)n->nbody));
            for (i = 0; i < n->nbody; i++) out.as.arr->v[i] = mx_eval(n->body[i], env);
            return out;
        }
        case N_INDEX: {
            MxValue base = mx_eval(n->a, env);
            MxValue idx  = mx_eval(n->b, env);
            if (base.tag == MX_ARR && idx.tag == MX_INT && (uint64_t)idx.as.i < (uint64_t)base.as.arr->n) {
                return base.as.arr->v[(size_t)idx.as.i];
            }
            return mx_index(base, idx);
        }
        case N_CALL: {
            int is_dot = (n->callee && n->callee->tag == N_DOT);
            if (!is_dot && mx_is_native_or_builtin_name(n->name)) {
                MxValue stack_args[16];
                MxValue *argv = (n->nargs <= 16) ? stack_args
                    : (MxValue *)arena_alloc((size_t)(n->nargs ? n->nargs : 1) * sizeof(MxValue));
                int ai;
                for (ai = 0; ai < n->nargs; ai++) argv[ai] = mx_eval(n->args[ai], env);

                MxValue native_v;
                int ni = n->nargs > MX_MAX_PARAMS ? MX_MAX_PARAMS : n->nargs;
                if (mx_try_native(n->name, argv, ni, &native_v))
                    return native_v;
                if (strcmp(n->name, "len") == 0) {
                    return mx_int(mx_len(n->nargs > 0 ? argv[0] : mx_null()));
                }
                if (strcmp(n->name, "typeofx") == 0) {
                    return mx_str(mx_type_name(n->nargs > 0 ? argv[0] : mx_null()));
                }
                if (strcmp(n->name, "readx") == 0 || strcmp(n->name, "writex") == 0 ||
                    strcmp(n->name, "appendx") == 0 || strcmp(n->name, "exportx") == 0) {
                    return mx_file_builtin(n->name, argv, n->nargs < 4 ? n->nargs : 4);
                }
                if (strcmp(n->name, "inputx") == 0) {
                    return mx_input(n->nargs > 0 ? argv[0] : mx_null());
                }
            }

            if (is_dot) {
                MxValue callee = mx_eval(n->callee, env);
                if (callee.tag == MX_FUNC) {
                    MxNode *mf = (MxNode *)callee.as.box;
                    MxValue stack_args[16];
                    MxValue *argv = (n->nargs <= 16) ? stack_args
                        : (MxValue *)arena_alloc((size_t)(n->nargs ? n->nargs : 1) * sizeof(MxValue));
                    int ai;
                    for (ai = 0; ai < n->nargs; ai++) argv[ai] = mx_eval(n->args[ai], env);
                    return mx_call_fn_list(mf, argv, n->nargs, g_globals);
                }
                if (callee.tag == MX_METHOD) {
                    MxBoundMethod *bm = (MxBoundMethod *)callee.as.box;
                    MxValue stack_args[16];
                    MxValue *argv = (n->nargs <= 16) ? stack_args
                        : (MxValue *)arena_alloc((size_t)(n->nargs ? n->nargs : 1) * sizeof(MxValue));
                    int ai;
                    for (ai = 0; ai < n->nargs; ai++) argv[ai] = mx_eval(n->args[ai], env);
                    return mx_call_instance(bm->method, argv, n->nargs,
                                            bm->instance, bm->klass);
                }
                mx_raise("'%s' is not callable", n->name);
                return mx_null();
            }

            {
                MxNode *fn = find_func(n->name);
                if (!fn) {
                    MxValue gv;
                    if (env_get(g_globals, n->name, &gv) && gv.tag == MX_FUNC)
                        fn = (MxNode *)gv.as.box;
                }
                if (!fn) mx_raise("'%s' is not callable", n->name);
                {
                    MxValue stack_args[16];
                    MxValue *argv = (n->nargs <= 16) ? stack_args
                        : (MxValue *)arena_alloc((size_t)(n->nargs ? n->nargs : 1) * sizeof(MxValue));
                    int i;
                    for (i = 0; i < n->nargs; i++) argv[i] = mx_eval(n->args[i], env);
                    return mx_call_fn_list(fn, argv, n->nargs, g_globals);
                }
            }
        }
        default:
            mx_unsupported("expr", n->line);
            return mx_null();
    }
}

static void mx_exec_stmt(MxNode *s, MxEnv *env) {
    if (s && s->line > 0) g_current_line = s->line;
    switch (s->tag) {
        case N_LET:   env_define(env, s->name, mx_eval(s->a, env)); return;
        case N_ASSIGN: {
            MxValue val = mx_eval(s->a, env);
            /* An index assignment's target is an N_INDEX node hanging off s->b,
             * so it is dispatched here rather than being a statement kind of its
             * own. The write path's error texts differ from the read path's
             * ("Array index must be an integer"), which is why mx_index_set is a
             * separate function and not mx_index with a flag. */
            /* A FIELD assignment: `this.f = v` or `obj.f = v`. The target is an
             * N_DOT whose object is the receiver (interpreter.py:415). The write
             * goes into the instance's OWN fields dict - never up the class
             * chain - because a field assignment is a per-object mutation, and
             * writing to the class would give every instance the new value.
             *
             * The host does NOT check the field exists: assigning a new name adds
             * it (dict assignment, :415), and reading it back later works. That is
             * why there is no "no such field" text in the sweep for this path. */
            if (s->b && s->b->tag == N_DOT) {
                MxValue recv = mx_eval(s->b->a, env);
                if (recv.tag != MX_INST) {
                    mx_raise("Cannot assign to field of non-instance");
                    return;
                }
                mx_dict_set(((MxInstance *)recv.as.box)->fields, s->b->field, val);
                return;
            }
            if (s->b && s->b->tag == N_INDEX) {
                /* ANY holder the host supports (interpreter.py:408-429
                 * assign_target): a bare variable, `this.f[i]` / `obj.f[i]`, or
                 * a nested `a[0][1]`. mx_eval(s->b->a) yields the ARRAY BOX the
                 * holder shares - the env binding, the instance field, or the
                 * outer array's element - and mx_index_set mutates that box IN
                 * PLACE (it has no growth path: it writes v[i] and returns the
                 * same array), so the write is visible through the holder with
                 * no write-back. The host does the same: Python's arr[idx] =
                 * value mutates the very list the target evaluated to.
                 *
                 * The old guard refused every holder that was not a bare
                 * N_IDENT - which is exactly hashtable.mx's put pattern
                 * (`this.buckets[bi] = b`) - and it raised AFTER the store had
                 * already mutated the box, so with the method boundary still
                 * swallowing signals the store stuck while every STATEMENT
                 * AFTER IT was skipped: size() == 0 with the entry present.
                 * Loud refusal would have been defensible; store-then-raise
                 * behind a swallow was silent truncation. Host is the
                 * specification, and the host accepts all three holders. */
                MxValue base = mx_eval(s->b->a, env);
                MxValue idx  = mx_eval(s->b->b, env);
                MxValue nv   = mx_index_set(base, idx, val);
                if (s->b->a->tag == N_IDENT) {
                    /* Rebind the name: the env slot still holds the old MxValue
                     * copy. Identity today (mx_index_set never moves), but the
                     * write-back is the contract the eval side reads back. */
                    env_set(env, s->b->a->name, nv);
                }
                return;
            }
            env_set(env, s->name, val);
            return;
        }
        /* --- CONTROL: the three loop/transfer statements ---
         * Return and Throw already exist below. Break and Continue set a KIND in
         * the shared signal slot and longjmp; the enclosing loop absorbs them.
         * They are not return values, because the loop has to distinguish
         * "stop" from "carry on" without either one looking like a normal
         * completion - and a value-returning form would have to encode that. */
        /* Break and Continue are REAL SIGNALS again, on the catch stack. They
         * spent this increment on a return-channel workaround because one
         * jmp_buf cannot hold nested catchers; with the stack they no longer
         * need it, and being signals is what makes them cross a tryx correctly -
         * the tryx's catcher is ABOVE the loop's on the stack, so a longjmp from
         * inside the tryx body would land in the LOOP, which is the innermost
         * live catcher, which is exactly the innermost-absorption rule. */
        case N_BREAK: {
            mx_raise_to(K_BREAK);
            return;
        }
        case N_CONTINUE: {
            mx_raise_to(K_CONTINUE);
            return;
        }
        case N_IF: {
            /* The host tests the condition ONCE and then picks a branch
             * (interpreter.py:314-316). An `else if` is an N_IF nested in the
             * else slot - there is no separate node kind - so this is the whole
             * chain, and an else-if chain of any depth costs one recursive call
             * per link. A missing `else` is simply else_body == NULL, which is
             * why the FALSY case must fall through to the next statement rather
             * than into a catch arm. */
            if (mx_fast_truthy(mx_eval(s->a, env))) {
                mx_exec_block(s->body, s->nbody, env);
            } else if (s->else_body) {
                /* The else slot holds EITHER a block (an N_PROGRAM, whose body
                 * list is the statements) OR - when the source said `else if` -
                 * a single N_IF node. Reading else_body->body for both is the bug
                 * this comment exists for: an N_IF has no body list, so it ran
                 * ZERO statements and the chain fell through, which printed the
                 * ELSE-IF's text for the wrong i and looked like a logic bug in
                 * the guest. Dispatching on the tag makes the two shapes
                 * explicit. The host has the same asymmetry and handles it by
                 * storing `else_b = [self.parse_if()]` - a one-element LIST
                 * (parser.py:178) - which is why the evaluator there can always
                 * execute a list. Here the slot is a node, so the tag is the
                 * discriminator. */
                if (s->else_body->tag == N_IF) {
                    mx_exec_stmt(s->else_body, env);
                } else {
                    mx_exec_block(s->else_body->body, s->else_body->nbody, env);
                }
            }
            return;
        }
        case N_WHILE: {
            /* THE LOOP, and the innermost consumer of K_BREAK / K_CONTINUE
             * (interpreter.py:318-325).
             *
             * With the catch STACK this is straightforward and correct by
             * construction: mx_try pushes THIS frame, so a break or continue from
             * anywhere inside - however deeply nested in ifs and tryx blocks -
             * longjmps to the top frame, which is the loop's own. The tryx frames
             * inside the body are pushed ABOVE it and popped before the signal
             * arrives, so none of them can intercept a break they do not own.
             * That is the innermost-absorption rule as an emergent property of
             * the stack, not as a hand-written check.
             *
             * The condition is evaluated OUTSIDE the mx_try on purpose: it must
             * not be inside, or a throw raised by the condition would be caught
             * by this loop and reported as if the loop had absorbed it. The host
             * re-tests the condition every iteration (interpreter.py:319), and a
             * `continue` means exactly "re-test" - the C continue.
             *
             * A signal this loop does NOT own (K_RETURN, K_THROW, K_ERROR) is
             * re-raised with mx_raise_to, which jumps to the NEXT frame down.
             * That is safe now: the next frame is a different function's or a
             * different block's, never this one, because mx_try popped on the way
             * out. Every earlier version of this code tried to re-raise with a
             * single buffer and spun forever; the stack is what makes it work. */
            MxEnv *volatile wenv = NULL;
            for (;;) {
                volatile int hit;
                if (!mx_fast_truthy(mx_eval(s->a, env))) break;
                if (!wenv) wenv = env_new(env);
                else wenv->n = 0;
                hit = mx_try(mx_exec_block(s->body, s->nbody, wenv));
                if (!hit) continue;          /* the body ran to completion */
                {   int kind = mx_caught_kind();
                    if (kind == K_BREAK)    { mx_clear_signal(); break; }
                    if (kind == K_CONTINUE) { mx_clear_signal(); continue; }
                    env_free(wenv);
                    mx_raise_to(kind);       /* not ours - hand it up */
                    return;
                }
            }
            if (wenv) env_free(wenv);
            return;
        }
        case N_LOOP: {
            /* The count ONCE, validated as an integer, then `max(0, n)`
             * iterations. `is_num` excluding bool is what makes `loop true()`
             * the host's "loop count must be an integer" rather than one
             * iteration - AGENTS.md's rule, seen from its other side.
             *
             * The signal handling is N_WHILE's verbatim, and for the same
             * reason: mx_try pushes THIS frame, so a break or continue from
             * anywhere in the body lands here, and a signal this loop does not
             * own is handed upward. The count is evaluated BEFORE the loop
             * enters mx_try, exactly as N_WHILE evaluates its condition outside
             * the frame - a throw while computing the count must not be
             * reported as if the loop had absorbed it. */
            MxValue cnt;
            /* i and times are read AFTER mx_try's longjmp, so both must be
             * volatile (C99 7.13.2.1p3) - -Wclobbered says so, and it is right:
             * without it the compiler may keep the counter in a register that
             * longjmp has not restored, and the loop either spins or stops
             * early. `volatile` is the fix CB4 made once for the same reason. */
            volatile int64_t i;
            int64_t times;
            volatile int hit;
            cnt = mx_eval(s->a, env);
            if (cnt.tag == MX_BOOL || !mx_is_num(cnt))
                mx_raise("loop count must be an integer");
            times = cnt.as.i;
            if (times < 0) times = 0;
            MxEnv *volatile lenv = NULL;
            for (i = 0; i < times; i++) {
                if (!lenv) lenv = env_new(env);
                else lenv->n = 0;
                hit = mx_try(mx_exec_block(s->body, s->nbody, lenv));
                if (!hit) continue;
                {   int kind = mx_caught_kind();
                    if (kind == K_BREAK)    { mx_clear_signal(); break; }
                    if (kind == K_CONTINUE) { mx_clear_signal(); continue; }
                    env_free(lenv);
                    mx_raise_to(kind);
                    return;
                }
            }
            if (lenv) env_free(lenv);
            return;
        }
        case N_FORIN: {
            /* `for x in xs` / `for i, v in xs` (interpreter.py:386-405).
             *
             * The rules that are easy to get wrong, all of them the host's:
             *   - the iterable is evaluated ONCE, before the loop. Re-evaluating
             *     it per iteration would loop forever on `for x in f()` and, more
             *     subtly, would re-read a variable the body rebinds.
             *   - only an ARRAY or a STRING iterates; anything else is
             *     "for...in expects an array or string" - NOT a truthiness test.
             *   - each iteration gets a FRESH env, so a body assignment to the
             *     loop variable does not leak into the next iteration's scope.
             *   - with two bindings the FIRST is the INDEX and the second the
             *     ELEMENT. It reads backwards; it is what the host does.
             *   - break/continue belong to THIS loop only, handled by the same
             *     catch-stack arrangement as N_WHILE and N_LOOP; a return or a
             *     throw is handed upward untouched.
             *
             * The element is read as an ARRAY INDEX, which is how a string
             * iterates BYTE - D-ARR1, the registered skip. That is inherited, not
             * chosen here, and case 20 is where it is pinned. */
            /* `it`, `count` and `idx` are all read AFTER mx_try's longjmp, so
             * all three must be volatile (C99 7.13.2.1p3). The compiler is right:
             * an ordinary local may live in a register longjmp never restores,
             * and the loop would then resume with a stale index or a freed
             * array. Same fix as N_LOOP and the same reason. */
            MxValue volatile it = mx_eval(s->a, env);
            volatile size_t count;
            volatile size_t idx;
            int is_str = (it.tag == MX_STR);
            if (!is_str && it.tag != MX_ARR)
                mx_raise("for...in expects an array or string");
            count = is_str ? strlen(it.as.s) : (size_t)it.as.arr->n;
            MxEnv *volatile iter_env = NULL;
            for (idx = 0; idx < count; idx++) {
                volatile int hit;
                if (!iter_env) iter_env = env_new(env);
                else iter_env->n = 0;
                if (s->var2) {
                    env_define(iter_env, s->name, mx_int((int64_t)idx));
                    env_define(iter_env, s->var2, mx_index(it, mx_int((int64_t)idx)));
                } else {
                    env_define(iter_env, s->name, mx_index(it, mx_int((int64_t)idx)));
                }
                hit = mx_try(mx_exec_block(s->body, s->nbody, iter_env));
                if (!hit) continue;
                {   int kind = mx_caught_kind();
                    if (kind == K_BREAK)    { mx_clear_signal(); break; }
                    if (kind == K_CONTINUE) { mx_clear_signal(); continue; }
                    env_free(iter_env);
                    mx_raise_to(kind);       /* return / throw / error: not ours */
                    return;
                }
            }
            if (iter_env) env_free(iter_env);
            return;
        }
        case N_PRINT: {
            MxValue v = mx_eval(s->a, env);
            if (v.tag == MX_STR) {
                printf("%s\n", v.as.s);
            } else {
                char buf[512];
                mx_display(v, buf, sizeof buf);
                printf("%s\n", buf);
            }
            return;
        }
        case N_EXPRSTMT: if (s->a) mx_eval(s->a, env); return;
        case N_THROW:
            /* mx_throw always unwinds (longjmp or exit), so there is no
             * fall-through in practice; the break is here because -Wextra
             * does not know that, and CB4's gate target is ZERO warnings. */
            mx_throw(mx_eval(s->a, env));
            break;
        case N_TRYCATCH: {
            /* The one place a signal is absorbed (interpreter.py:357-360).
             * mx_try is the ONLY setjmp, so a missed catcher is a compile
             * error rather than a forgotten `if`. */
            /* The SAME trio a generated program uses, so mini.c and the emitted
             * code share ONE error discipline and it is exercised twice. If the
             * trio is wrong, B-1's cases 05/06/09/10/11 fail alongside the B-2
             * equivalents. That duplication is the point. */
            /* TRY AGAIN once the loop has declined a signal. The loop cannot
             * re-raise (one jmp_buf holds one catcher), so it RETURNS with the
             * signal still set; this catcher then needs a second setjmp at THIS
             * frame to pick it up. Without it a throw from inside a while is
             * silently swallowed by the loop and the tryx runs its catch body
             * with no signal at all - which is why /tmp case 19 printed 0 where
             * the host printed "caught negative at 1".
             *
             * The retry is INSIDE the same statement and after the loop returned,
             * so the setjmp buffer is fresh and g_has_catch is set again by the
             * macro. Nothing here re-arms a foreign frame. */
            /* Start from a CLEAN channel. A loop that declined a throw did so by
             * RETURNING, which leaves g_sig.kind set, and the next tryx in the
             * program then saw that stale kind and ran its catch arm on a body
             * that never threw - an extra line of output, in the wrong place,
             * with no error anywhere. The host cannot have this problem because
             * a Python exception is consumed by its handler. */
            MxEnv *volatile tenv = env_new(env);
            volatile int caught = mx_try(mx_exec_block(s->a->body, s->a->nbody, tenv));
            env_free(tenv);
            if (caught) {
                /* Break and Continue RE-THROW, always. A tryx absorbs Return and
                 * Throw and nothing else (AGENTS.md's CONTROL_FLOW invariant), so
                 * a `break` inside a guarded block belongs to the enclosing LOOP,
                 * not to this catcher. Without this re-raise the signal is
                 * swallowed here, the catch body runs with an empty value, and the
                 * loop runs to completion - a wrong ANSWER, and one that corpus
                 * case 16 exists to catch. The `volatile` on `caught` is what the
                 * C standard requires after a longjmp for the value to survive. */
                /* Break and Continue are NOT longjmps, and this comment is the
                 * reason. They were first implemented as kinds in the shared
                 * signal and re-raised with mx_throw_kind, which needs the
                 * catcher flag RE-ARMED - and re-arming makes THIS setjmp's own
                 * frame live again, so the tryx catches its own re-raised signal
                 * and spins forever. One jmp_buf cannot hold a stack of catchers,
                 * which is the same constraint the host solves with a Python
                 * exception.
                 *
                 * So they are RETURN VALUES instead: mx_exec_stmt and
                 * mx_exec_block propagate the kind up the C stack, and only the
                 * loop consumes it. A `break` inside a tryx inside a while then
                 * just returns K_BREAK out of the block, through the tryx (which
                 * passes it along untouched), into the loop. No jump target is
                 * involved, so there is nothing to re-arm and nothing to unwind
                 * past. RETURN and THROW stay on the longjmp channel: they cross
                 * arbitrarily many frames, and only the setjmp can do that. */
                /* A tryx absorbs THROW and ERROR - and NOTHING ELSE
                 * (interpreter.py:357-360, and AGENTS.md's CONTROL_FLOW
                 * invariant). K_BREAK and K_CONTINUE belong to the enclosing
                 * LOOP; K_RET belongs to the enclosing FUNCTION. All three are
                 * handed up with mx_rethrow, which delivers to the next catcher
                 * down - the loop, or the function boundary.
                 *
                 * Getting this wrong is a wrong ANSWER rather than a crash: the
                 * first version ran the catch arm for a K_RET, so `guarded()`
                 * returned -2 where the host returned 7, and a `break` inside a
                 * guarded block never reached its loop. Case 16 and case 19 exist
                 * because of those two wrong answers. */
                if (mx_caught_kind() != K_THROW && mx_caught_kind() != K_ERROR) {
                    mx_rethrow();
                    return;
                }
                /* A THROW hands the catcher the raw VALUE; an ERROR hands it a
                 * STRING (interpreter.py:358 vs :360). Two kinds, because the
                 * top-level renderer tells them apart even though this cannot. */
                MxEnv *cenv = env_new(env);
                MxValue cv = (mx_caught_kind() == K_THROW) ? mx_caught_value()
                                                          : mx_str(mx_caught_msg());
                env_define(cenv, s->catch_var, cv);
                /* Clear AFTER the catch body, not before. Clearing first wipes
                 * g_sig.msg, and the body's `e` reads the empty string - which
                 * is what case 09 caught. The signal must outlive the catch. */
                mx_exec_block(s->catch_body->body, s->catch_body->nbody, cenv);
                env_free(cenv);
                mx_clear_signal();
            }
            return;
        }
        case N_CLASSDEF:
            /* A class REGISTERS at execution, not at parse time
             * (interpreter.py:249-250). That is deliberate: a class defined
             * INSIDE a function body still registers, which is what AGENTS.md
             * records as the fix for "Unknown stmt ClassDef" - classes inside
             * blocks had to register too.
             *
             * The registration is a RUNTIME dictionary write, not a vtable
             * emission, and that is COMPILE_DESIGN 8.2's whole point: a class may
             * be defined after a function that mentions it was COMPILED. */
            mx_define_class(s->name, s);
            return;
        case N_IMPORT:
            return mx_exec_import(s, env);
        case N_FIELD:
            /* a FIELD is only ever reached through ClassDef's merge, never as a
             * statement of its own */
            mx_raise("Unexpected field outside a class body");
            return;
        case N_FUNCDEF:
            if (g_nfuncs < 32) g_funcs[g_nfuncs++] = s;
            return;
        case N_RETURN:
            /* `return` inside a NESTED block - inside an `if`, a loop, a tryx -
             * has to leave the FUNCTION, not just the block. The longjmp channel
             * is how: K_RET with the value in g_sig.value, caught in
             * mx_call_fn_list. The first implementation RAISED "'return' used
             * outside a function" for any nested return, which meant `return` was
             * only legal as a function's last top-level statement - and case 19's
             * `return` from inside a while HUNG, because the raise happened after
             * the loop had already spun. A control transfer that cannot cross a
             * block boundary is not a return.
             *
             * mx_call_fn_list already checks g_sig.value for K_RET; this only has
             * to raise it. The `volatile` discipline is unchanged - the flag that
             * must survive the longjmp is the KIND, which lives in a global. */
            /* A real signal again, with the value in the payload. The catch stack
             * carries it across every intervening loop and tryx, each of which
             * declines it via mx_rethrow, and mx_call_fn_list - which pushed NO
             * frame of its own - is the boundary that catches it. */
            g_sig.value = s->a ? mx_eval(s->a, env) : mx_null();
            mx_raise_to(K_RET);
            return;
        default:
            mx_unsupported("stmt", s->line);
            return;
    }
}

static void mx_exec_block(MxNode **body, int n, MxEnv *env) {
    int i;
    /* STOP at a pending Break/Continue. Everything after the break in this block
     * must not run, and the signal itself is left SET so the enclosing loop can
     * see it - the loop clears it. This is the "innermost absorption" rule:
     * the nearest enclosing LOOP takes it, and a tryx in between passes it
     * through untouched because its own catcher only inspects g_sig. */
    for (i = 0; i < n; i++) {
        /* STOP on a pending break/continue OR return. Both are return-channel
         * signals: whatever follows them in this block must not run, and the
         * signal stays set for the loop or the function to consume. */
        if (g_loop_sig != K_NONE) return;
        mx_exec_stmt(body[i], env);
    }
}

/* ===================================================================== */
/* 9. Entry - main.py's auto-wrap and exit codes.                        */
/* ===================================================================== */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) { fprintf(stderr, "Cannot read '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)arena_alloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read\n"); exit(1);
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* The AST half's OWN entry point. Compiled out in a B-2 (MX_BOTH) binary, whose
 * entry point is the EMITTED main() - a generated program is not the interpreter
 * walking a corpus file. Two `main`s in one link is a hard error, and the fix
 * belongs HERE rather than in the generated file, because the generated file
 * knows nothing about mini.c and must not have to. Everything this main does -
 * running a corpus case - is still available in a B-2 binary through
 * mx_run_source, which is what the AST half's body calls. */
#ifndef MX_BOTH
int main(int argc, char **argv) {
    char *src;
    Token *toks;
    P p;
    MxNode *prog;
    MxEnv *globals;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("MountX 1.1.0  (c) 2026 Ziad Mohsin\n");
        return 0;
    }
    if (argc < 2) { fprintf(stderr, "usage: mountx <file.mx>\n"); return 1; }
    src = read_file(argv[1]);

    toks = lex(src);
    /* main.py:30-32 auto-wraps a snippet whose first token is not MOUNTX. The
     * corpus files all carry the wrapper, so this only has to accept it. */
    p.t = toks; p.i = 0;
    if (peek(&p)->type == T_KW && strcmp(peek(&p)->text, "mountx") == 0) p.i++;

    prog = parse_body(&p, NULL);
    if (peek(&p)->type == T_KW && strcmp(peek(&p)->text, "unmountx") == 0) p.i++;

    globals = env_new(NULL);
    g_globals = globals;
    /* SAFETY NETS, before anything can run. The order matters only in that they
     * must precede the first statement: a net installed after the program has
     * already looped protects nothing. */
    mx_install_safety_nets();
    mx_set_argv_full(argv, argc);
    /* The imported set is allocated HERE, in main, not lazily at the first
     * importx: `mx_dict_has(NULL, ...)` dereferences a null dict and every
     * `importx` segfaulted, including one for a module that does not exist. The
     * set must exist before the FIRST import, which is the only import that
     * cannot check for it. */
    mx_imported_set();   /* allocated up front too: the first import is the one
                          * import that cannot check, so it must not be the one
                          * that allocates either */
    /* g_script_dir is the ENTRY SCRIPT's directory - the second import base
     * (:200) and, with MOUNTX_STD unset, the FIRST one. It is derived from
     * argv[1] at RUN time, exactly as the host derives it from its own
     * `script_dir`, and that is what makes a compiled interpreter resolve
     * imports relative to the program the user ran (D-1's shape).
     *
     * Without it the walk started at the CWD, so `importx` found a different
     * module whenever the two directories differed - and the divergence showed
     * up ONLY in the not-found text, which is the one message that lists every
     * path searched. */
    if (argc > 1) {
        static char sdir[1024];
        char *slash;
        snprintf(sdir, sizeof sdir, "%s", argv[1]);
        slash = strrchr(sdir, '/');
        if (slash) *slash = '\0'; else snprintf(sdir, sizeof sdir, ".");
        g_script_dir = sdir;
    }
    mx_set_argv0(argv[0]);
    g_globals = globals;
    mx_exec_block(prog->body, prog->nbody, globals);
    fflush(stdout);
    return 0;
}
#endif /* MX_BOTH - a generated program's main() is emitted, not this one */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
int mountx_run(const char *code) {
    if (!code || !*code) return 0;
    Token *toks = lex(code);
    P p;
    MxNode *prog;
    MxEnv *globals;

    if (!toks || !mx_first_is_mountx(toks)) {
        size_t need = strlen(code) + 24;
        char *wrapped = (char *)arena_alloc(need);
        snprintf(wrapped, need, "mountx\n%s\nunmountx\n", code);
        p.t = lex(wrapped);
    } else {
        p.t = toks;
    }
    p.i = 0;
    if (peek(&p)->type == T_KW && strcmp(peek(&p)->text, "mountx") == 0) p.i++;

    prog = parse_body(&p, NULL);
    if (peek(&p)->type == T_KW && strcmp(peek(&p)->text, "unmountx") == 0) p.i++;

    globals = env_new(NULL);
    g_globals = globals;
    mx_install_safety_nets();
    mx_imported_set();

    volatile int hit = mx_try(mx_exec_block(prog->body, prog->nbody, globals));
    if (hit) {
        mx_raise_pending();
        mx_clear_signal();
        fflush(stdout);
        fflush(stderr);
        return 1;
    }
    fflush(stdout);
    fflush(stderr);
    return 0;
}
#endif


static MxNode *parse_primary(P *p) {
    Token *t = next(p);
    if (t->type == T_INT)  { MxNode *n = node(N_LITERAL); n->lit = mx_int(t->i);  n->line = t->line; return n; }
    if (t->type == T_NUM)  { MxNode *n = node(N_LITERAL); n->lit = mx_num(t->d);  n->line = t->line; return n; }
    if (t->type == T_STR)  { MxNode *n = node(N_LITERAL); n->lit = mx_str(arena_strdup(t->text, strlen(t->text))); n->line = t->line; return n; }
    if (t->type == T_KW && strcmp(t->text, "true") == 0)  { MxNode *n = node(N_LITERAL); n->lit = mx_bool(1); n->line = t->line; return n; }
    if (t->type == T_KW && strcmp(t->text, "false") == 0) { MxNode *n = node(N_LITERAL); n->lit = mx_bool(0); n->line = t->line; return n; }
    if (t->type == T_KW && (strcmp(t->text, "nullx") == 0 ||
                            strcmp(t->text, "null")  == 0 ||
                            strcmp(t->text, "nil")   == 0)) { MxNode *n = node(N_LITERAL); n->lit = mx_null();  n->line = t->line; return n; }
    if (t->type == T_IDENT) { MxNode *n = node(N_IDENT); n->name = arena_strdup(t->text, strlen(t->text)); n->line = t->line; return n; }
    if (t->type == T_KW && strcmp(t->text, "newx") == 0) {
        /* `newx Name(args)` or `newx mod.Name(args)`. The class name is kept as
         * ONE string including any dot, because resolve_class decides at CALL
         * time whether it is a plain name or a dotted module lookup
         * (interpreter.py:639-647) - and it must decide that AFTER the args are
         * evaluated, so the name is never resolved here. */
        MxNode *n = node(N_NEW);
        char namebuf[128];
        size_t used = 0;
        /* NO p->i++ here: parse_primary opened with `Token *t = next(p)`, which
         * ALREADY consumed `newx`. The extra increment skipped the class NAME,
         * so the scan below saw `(` and every `newx` reported "Expected class
         * name". That is the same class of slip as B-3.2's keyword arms, and the
         * same lesson: this parser PEEKS in parse_stmt and CONSUMES in
         * parse_primary, so a branch's first act depends on which function it is
         * in. The comment on the keyword arms says so; this one did not. */
        n->line = t->line;
        {   /* The class name comes BEFORE the parenthesis - `newx B(1)`, not
             * `newx (B)(1)`. Reading it after expect_punct("(") consumed the
             * open paren as part of the name scan, left `used` at zero, and
             * every `newx` resolved the empty class name: "Class '' not
             * defined". The name is read as idents and dots, so a dotted
             * `newx mod.Name()` arrives as ONE string for resolve_class to
             * split at CALL time (interpreter.py:639-647). */
            for (;;) {
                Token *nt = peek(p);
                if (nt->type == T_IDENT || (nt->type == T_KW)) {
                    size_t l = strlen(nt->text);
                    if (used + l + 1 < sizeof namebuf) {
                        memcpy(namebuf + used, nt->text, l);
                        used += l;
                    }
                    p->i++;
                } else if (at_punct(p, ".")) {
                    if (used + 1 < sizeof namebuf) namebuf[used++] = '.';
                    p->i++;
                } else {
                    break;
                }
            }
            namebuf[used] = '\0';
            if (used == 0) mx_raise("Expected class name after newx at line %d", n->line);
            n->class_name = arena_strdup(namebuf, used);
        }
        expect_punct(p, "(");
        {
        }
        n->nargs = 0;
        n->args = (MxNode **)arena_alloc(4 * sizeof(MxNode *));
        if (!at_punct(p, ")")) {
            for (;;) {
                if (n->nargs >= 4) {
                    MxNode **na = (MxNode **)arena_alloc(
                        (size_t)(n->nargs * 2) * sizeof(MxNode *));
                    int bi; for (bi = 0; bi < n->nargs; bi++) na[bi] = n->args[bi];
                    n->args = na;
                }
                n->args[n->nargs++] = parse_expr(p);
                if (!at_punct(p, ",")) break;
                p->i++;
            }
        }
        expect_punct(p, ")");
        return n;
    }
    if (t->type == T_KW && strcmp(t->text, "this") == 0) {
        /* `this` is an IDENT-like expression, not a variable, so it becomes a
         * call of a reserved name the evaluator resolves to the receiver. That
         * keeps it out of the environment, which is right: `this` is not a
         * binding a program can rebind. */
        /* NO p->i++: parse_primary's `next(p)` already consumed `this`. The
         * extra increment skipped the FOLLOWING token, so `typeofx(this)` parsed
         * as `typeofx()` and reported "Expected )". This is the third instance
         * of one slip - `newx` had it too - and it is worth naming: in this
         * parser parse_stmt PEEKS and parse_primary CONSUMES, so a branch's
         * first act depends entirely on which function it sits in. */
        MxNode *n = node(N_IDENT);
        n->name = arena_strdup("this", 4);
        n->line = t->line;
        return n;
    }
    if (t->type == T_KW && strcmp(t->text, "super") == 0) {
        MxNode *n = node(N_SUPER);
        n->line = t->line;      /* no p->i++ - see the `this` arm above */
        return n;
    }
    if (t->type == T_KW && strcmp(t->text, "typeofx") == 0) {
        expect_punct(p, "(");
        MxNode *n = node(N_TYPEOF); n->a = parse_expr(p);
        expect_punct(p, ")"); n->line = t->line; return n;
    }
    if (t->type == T_PUNCT && strcmp(t->text, "(") == 0) {
        MxNode *n = parse_expr(p);
        expect_punct(p, ")");
        return n;
    }
    if (t->type == T_PUNCT && strcmp(t->text, "[") == 0) {
        /* An array literal: elements are re-used as the generic node LIST
         * (body/nbody), because MxNode already has one and a second list type
         * would be duplication for its own sake. The list is BUILT at EVALUATION
         * time, not here - see the N_ARRAYLIT arm, and why the fresh stamp
         * matters. */
        MxNode *n = node(N_ARRAYLIT);
        n->line = t->line;
        n->nbody = 0;
        if (!at_punct(p, "]")) {
            for (;;) {
                MxNode **nb = (MxNode **)arena_alloc(
                    (size_t)(n->nbody + 1) * sizeof(MxNode *));
                int i;
                for (i = 0; i < n->nbody; i++) nb[i] = n->body[i];
                nb[n->nbody] = parse_expr(p);
                n->body = nb;
                n->nbody = n->nbody + 1;
                if (!at_punct(p, ",")) break;
                p->i++;
            }
        }
        expect_punct(p, "]");
        return n;
    }
    mx_raise("Unexpected token %s at line %d", t->text, t->line);
    return NULL;   /* unreachable; keeps -Wall quiet */
}

static MxNode *parse_postfix(P *p, MxNode *left) {
    /* This LOOPS, and that is the fix for a real defect. It used to handle at
     * most one postfix and return, so `a[len(a) - 1]` parsed as `a[` then the
     * argument `len(a) - 1` — no. It parsed `len` as the argument, stopped at
     * `(`'s closing paren, and then expected `]` on a `1`. Corpus case 14 line 9
     * caught it with "Expected ] at line 9", which is a confusing message for a
     * non-recursive loop.
     *
     * The loop is what makes the precedence honest: postfix binds tighter than
     * every binop, so ANY number of `[...]` and `(...)` applications belong
     * here, left to right, before parse_expr ever sees an operator. */
    for (;;) {
        /* Field access: expr.field. Handled in POSTFIX position so it binds
         * tighter than every binop - `a.b + c.d` is (a.b) + (c.d) - and so
         * `newx Foo().m` works, the receiver being a complete primary. */
        if (at_punct(p, ".")) {
            MxNode *n = node(N_DOT);
            Token *f;
            n->a = left;
            n->line = left->line;
            p->i++;
            f = next(p);
            if (f->type != T_IDENT && f->type != T_KW)
                mx_raise("Expected field name after '.' at line %d", f->line);
            n->field = arena_strdup(f->text, strlen(f->text));
            left = n;
            continue;
        }
        /* Indexing: expr[i] */
        if (at_punct(p, "[")) {
            MxNode *n = node(N_INDEX);
            n->a = left;
            n->line = left->line;
            expect_punct(p, "[");
            n->b = parse_expr(p);
            expect_punct(p, "]");
            left = n;
            continue;
        }
        /* Call. The callee may be an IDENT (a plain function, found by name) or
         * an N_DOT (a method, already resolved to a bound method). Only an
         * IDENT used to qualify, so `a.go()` failed with "Expected )" on the
         * closing paren - the name after the dot had already been consumed and
         * nothing was left to close.
         *
         * The N_CALL's `name` slot is the FUNCTION NAME, which a dotted callee
         * does not have - so it is filled with the FIELD name purely so the
         * "not callable" text has something to quote. That is a display
         * convenience, not a lookup key. */
        if ((left->tag == N_IDENT || left->tag == N_DOT) && at_punct(p, "(")) {
            MxNode *n = node(N_CALL);
            n->name = (left->tag == N_IDENT) ? left->name : left->field;
            n->line = left->line;
            n->callee = left;         /* the callee NODE, for the method path -
                                       * a DEDICATED slot, because `a` aliases
                                       * args[0] below and reusing it made
                                       * every method call report
                                       * "'go' is not callable" */
            expect_punct(p, "(");
            /* A LIST of arguments, for the same reason the parameter list is:
             * case 19 is the first two-argument call in the corpus and interp.mx
             * has 685 of them. The single `n->a` this replaces stopped at the
             * comma and reported "Expected ) at line 21", naming the closing
             * paren rather than the missing list handling. */
            {
                int acap = 4;
                n->nargs = 0;
                n->args = (MxNode **)arena_alloc((size_t)acap * sizeof(MxNode *));
                while (!at_punct(p, ")")) {
                    if (n->nargs >= acap) {
                        MxNode **na = (MxNode **)arena_alloc(
                            (size_t)acap * 2 * sizeof(MxNode *));
                        int bi;
                        for (bi = 0; bi < acap; bi++) na[bi] = n->args[bi];
                        n->args = na;
                        acap = acap * 2;
                    }
                    n->args[n->nargs++] = parse_expr(p);
                    if (!at_punct(p, ",")) break;
                    p->i++;
                }
                /* n->a still points at the FIRST argument, so the single-argument
                 * `mx_call_fn` path and the `len`/`typeofx` natives keep working
                 * unchanged - one representation, two readers. */
                if (n->nargs >= 1) n->a = n->args[0];
            }
            expect_punct(p, ")");
            left = n;
            continue;
        }
        return left;
    }
}

/* parse_unary - the tightest level. A unary operator binds tighter than
 * multiplicative, which is why `-a * b` is `(-a) * b` and not `-(a * b)`.
 * The first draft folded the minus into the LITERAL, which cannot express `-a`
 * at all; this emits a real N_UNARY node and lets the substrate negate, so the
 * int/float tag survives (display renders 1 and 1.0 differently). */
static MxNode *parse_unary(P *p) {
    if (at_punct(p, "-")) {
        MxNode *t;
        p->i++;
        t = parse_unary(p);
        {   MxNode *u = node(N_UNARY);
            u->op = arena_strdup("MINUS", 5);
            u->a = t;
            u->line = t->line;
            return u;
        }
    }
    if (at_punct(p, "!")) {
        MxNode *t;
        p->i++;
        t = parse_unary(p);
        {   MxNode *u = node(N_UNARY);
            u->op = arena_strdup("NOT", 3);
            u->a = t;
            u->line = t->line;
            return u;
        }
    }
    return parse_postfix(p, parse_primary(p));
}

static MxNode *parse_multiplicative(P *p) {
    MxNode *left = parse_unary(p);
    for (;;) {
        if (op_at(p, "*") || op_at(p, "/") || op_at(p, "%")) {
            Token *o = next(p);
            MxNode *n = node(N_BINOP);
            n->op = arena_strdup(ast_op(o->text), 5);
            n->a = left;
            n->b = parse_unary(p);
            n->line = o->line;
            left = n;
            continue;
        }
        return left;
    }
}

static MxNode *parse_additive(P *p) {
    MxNode *left = parse_multiplicative(p);
    for (;;) {
        if (op_at(p, "+") || op_at(p, "-")) {
            Token *o = next(p);
            MxNode *n = node(N_BINOP);
            n->op = arena_strdup(ast_op(o->text), 5);
            n->a = left;
            n->b = parse_multiplicative(p);
            n->line = o->line;
            left = n;
            continue;
        }
        return left;
    }
}

static MxNode *parse_comparison(P *p) {
    MxNode *left = parse_additive(p);
    for (;;) {
        if (op_at(p, "==") || op_at(p, "!=") || op_at(p, ">") || op_at(p, "<") ||
            op_at(p, ">=") || op_at(p, "<=")) {
            Token *o = next(p);
            MxNode *n = node(N_BINOP);
            n->op = arena_strdup(ast_op(o->text), 5);
            n->a = left;
            n->b = parse_additive(p);
            n->line = o->line;
            left = n;
            continue;
        }
        return left;
    }
}

static MxNode *parse_logical_and(P *p) {
    MxNode *left = parse_comparison(p);
    while (op_at(p, "&&")) {
        Token *o = next(p);
        MxNode *n = node(N_BINOP);
        n->op = arena_strdup("AND", 3);
        n->a = left;
        n->b = parse_comparison(p);
        n->line = o->line;
        left = n;
    }
    return left;
}

static MxNode *parse_logical_or(P *p) {
    MxNode *left = parse_logical_and(p);
    while (op_at(p, "||")) {
        Token *o = next(p);
        MxNode *n = node(N_BINOP);
        n->op = arena_strdup("OR", 2);
        n->a = left;
        n->b = parse_logical_and(p);
        n->line = o->line;
        left = n;
    }
    return left;
}

/* parse_expression is the host's NAME for the top of the ladder
 * (parser.py:254-255), so it is the top here too - the ladder is only correct
 * if the ENTRY point is the loosest level, and starting higher would silently
 * make `a || b && c` mean `a || (b && c)` for the wrong reason. */
static MxNode *parse_expr(P *p) {
    return parse_logical_or(p);
}


#endif /* MX_SUBSTRATE */

#ifdef MX_SUBSTRATE
/* The CALLING CONVENTION for generated functions.
 *
 * Every emitted function has the same signature -
 *     static MxValue mx_fn_<name>_gen(MxValue p0_, MxValue p1_, ..., int mx_arity)
 * - so arity is a RUNTIME argument rather than a C signature, and one call site
 * serves every arity. That uniformity is what keeps the emitter simple: it never
 * has to know how many parameters a function has in order to call it.
 *
 * The PARAMETERS are by position, and the names live in the generated frame: each
 * function's prologue does env_define(env, "name", p0_) for each parameter it
 * declared. The substrate never sees parameter NAMES, which is why one convention
 * works for every function in every generated program.
 *
 * The extra ARITY argument is what lets a zero-parameter function exist: a C
 * function cannot be called with zero arguments when its signature declares
 * some, and the host's `func f() { }` must work. So the frame takes a fixed few
 * and is told how many are real; the prologue ignores the rest. arity is capped
 * at MX_MAX_PARAMS and a larger call is REFUSED by name rather than truncated -
 * a silently dropped argument is a wrong answer, and interp.mx has functions
 * with more parameters than any fixed signature would hold. */
MXFN MxValue mx_call_fn_ptr(MxFnGen fn, MxValue p0, MxValue p1, MxValue p2, MxValue p3, MxValue p4, MxValue p5, int nargs) {
    if (nargs > MX_MAX_PARAMS) {
        mx_raise("call has %d arguments, more than the %d the substrate passes",
                 nargs, MX_MAX_PARAMS);
    }
    /* Evaluated left to right HERE, not at the call site. C leaves the order of
     * function arguments unspecified, so a program whose two arguments both
     * print (or one of which throws) would pass or fail by compiler mood. */
    return fn(p0, p1, p2, p3, p4, p5, nargs);
}
#endif /* MX_SUBSTRATE - only generated code calls through a pointer */

/* ---- the GENERATED path's class registry ------------------------------
 * All of it is inside one #ifdef MX_SUBSTRATE, because a standalone B-1 build
 * has no generated code: leaving the declarations outside made them static-and-
 * unused, and -Wall reported four "declared but never defined" warnings that
 * were really a misplaced guard. */


