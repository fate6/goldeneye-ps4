/* libGE64Radv.prx glue (tools_ps4/build-radv-prx.sh): its exports besides
 * vk_icdGetInstanceProcAddr, and the process runtime pieces a module doesn't
 * get from the eboot's startup code.
 *
 *  - Heap: the static libc's malloc uses the system libc heap, which only the
 *    eboot's crt1 sets up (and which can't initialise on retail consoles
 *    anyway); in the module it is NULL. The module links with --wrap for the
 *    malloc family and forwards to the eboot's heap (DolphinPS4's runtime),
 *    handed over with ge64_radv_set_heap() right after loading. Allocations
 *    made before that (the module's own constructors) come from a small
 *    static arena whose blocks are never freed.
 *  - getenv: the static libc's environment is set up by the eboot's startup
 *    code too; the module reads the eboot's (which env.txt fills).
 *  - Program name and environment pointer (musl's __progname, which
 *    program_invocation_short_name aliases, and __environ): set by the
 *    eboot's crt1 only; Mesa reads the name (u_process.c). musl's full
 *    __init_libc would also redo TLS and the standard fds, so only these.
 *  - Constructors: crtlib's module_start never runs the module's .init_array
 *    (ACO's opcode table); ge64_radv_set_heap() does, once the heap is in.
 *  - pthread_once: the kernel's writes 16 bytes into the 4-byte
 *    pthread_once_t of the OpenOrbis headers (DolphinPS4 port/ps4_runtime.cpp).
 */
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);

EXPORT int ge64_radv_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg)
{
    return ac_ps4_submit_flip(video, buffer, mode, arg);
}

/* ---- heap ---------------------------------------------------------------- */

struct Ge64RadvHeap {
    void *(*malloc)(size_t);
    void (*free)(void *);
    void *(*calloc)(size_t, size_t);
    void *(*realloc)(void *, size_t);
    void *(*memalign)(size_t, size_t);
    char *(*getenv)(const char *);
};

static struct Ge64RadvHeap g_heap;

extern char *__progname, *__progname_full;
extern char **__environ;

/* The module's real constructor list (marked in build-radv-prx.sh's link.x). */
typedef void (*Ge64Ctor)(void);
extern Ge64Ctor __ge64_init_array_start[], __ge64_init_array_end[];

EXPORT void ge64_radv_set_heap(const struct Ge64RadvHeap *heap)
{
    static char name[] = "GoldenEye64";
    static char *no_env[] = {NULL};
    static int constructed;
    g_heap = *heap;
    __progname = __progname_full = name;
    if (!__environ) __environ = no_env;
    /* crtlib's module_start never runs them (see build-radv-prx.sh). */
    if (!constructed) {
        constructed = 1;
        for (Ge64Ctor *c = __ge64_init_array_start; c < __ge64_init_array_end; c++) (*c)();
    }
}

/* Early arena: 16-byte header holding the block size, then the block. */
#define ARENA_SIZE (4u << 20)
static uint8_t g_arena[ARENA_SIZE] __attribute__((aligned(64)));
static size_t g_arena_used;

static int in_arena(const void *p)
{
    return (const uint8_t *)p >= g_arena && (const uint8_t *)p < g_arena + ARENA_SIZE;
}

static void *arena_alloc(size_t size, size_t align)
{
    if (align < 16) align = 16;
    size_t start = (g_arena_used + 16 + align - 1) & ~(align - 1);
    if (start + size > ARENA_SIZE) return NULL;
    *(size_t *)(g_arena + start - 16) = size;
    g_arena_used = start + size;
    return g_arena + start;
}

void *__wrap_malloc(size_t size)
{
    return g_heap.malloc ? g_heap.malloc(size) : arena_alloc(size, 16);
}

void __wrap_free(void *p)
{
    if (!p || in_arena(p)) return;
    if (g_heap.free) g_heap.free(p);
}

void *__wrap_calloc(size_t n, size_t size)
{
    if (g_heap.calloc) return g_heap.calloc(n, size);
    void *p = arena_alloc(n * size, 16);
    if (p) memset(p, 0, n * size);
    return p;
}

void *__wrap_memalign(size_t align, size_t size)
{
    return g_heap.memalign ? g_heap.memalign(align, size) : arena_alloc(size, align);
}

void *__wrap___memalign(size_t align, size_t size)
{
    return __wrap_memalign(align, size);
}

void *__wrap_realloc(void *p, size_t size)
{
    if (p && in_arena(p)) {
        void *n = __wrap_malloc(size);
        if (n) {
            size_t old = *(size_t *)((uint8_t *)p - 16);
            memcpy(n, p, old < size ? old : size);
        }
        return n;
    }
    if (g_heap.realloc) return g_heap.realloc(p, size);
    return p ? NULL : arena_alloc(size, 16);
}

char *__wrap_getenv(const char *name)
{
    return g_heap.getenv ? g_heap.getenv(name) : NULL;
}

/* ---- pthread_once (4-byte state: 0 not run, 1 running, 2 done) ----------- */

int __wrap_pthread_once(int *once, void (*init)(void))
{
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(once, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) sched_yield();
    return 0;
}
