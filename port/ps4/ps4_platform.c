/*
 * PS4 platform layer: startup, logging, fixed low mappings, low stacks.
 *
 * Console facts (measured with probes/probe-lowmem, fw 9.00):
 *  - the eboot loads at 0x400000; nothing else is mapped below ~0x7_EEBF_4000;
 *  - anonymous and MAP_SHARED file mappings at 0x10000000, 0x30000000,
 *    0x70000000 and 0x80000000 work, and two views of one unlinked /data
 *    file alias;
 *  - calling into a system module that was not loaded with
 *    sceSysmoduleLoadModuleInternal dies with PRX_NOT_RESOLVED_FUNCTION;
 *  - returning from main() is reported as a crash (CE-34878-0).
 */

#include "ps4_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    void *start;
    void *end;
    off_t offset;
    int32_t protection;
    int32_t memory_type;
    unsigned is_flexible : 1;
    unsigned is_direct : 1;
    unsigned is_stack : 1;
    unsigned is_pooled : 1;
    unsigned is_committed : 1;
    char name[32];
} Ps4VQInfo;

int sceKernelVirtualQuery(const void *addr, int flags, Ps4VQInfo *info, size_t size);
int sceSysmoduleLoadModuleInternal(uint32_t id);

/* SceSystemServiceStatus (OpenOrbis declares the function without it). */
typedef struct {
    int32_t eventNum;
    uint8_t isSystemUiOverlaid;
    uint8_t isInBackgroundExecution;
    uint8_t isCpuMode7CpuNormal;
    uint8_t isGameLiveStreamingOnAir;
    uint8_t isOutOfVrPlayArea;
    uint8_t reserved[125];
} Ps4SystemServiceStatus;
int sceSystemServiceGetStatus(Ps4SystemServiceStatus *status);

/* DolphinPS4's runtime wraps mmap (large anonymous maps go to the system
 * flexible pool at an address of its choosing); fixed maps need the real one. */
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);

extern char __text_start[];


static int s_logFd = -1;

void ps4LogWrite(const char *line, size_t len)
{
    if (s_logFd >= 0) {
        write(s_logFd, line, len);
    }
}

static void ps4Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ps4Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    ps4LogWrite(buf, (size_t)n);
}

/* First mapping at or above addr; 0 if none. */
static int ps4NextMapping(uintptr_t addr, Ps4VQInfo *info)
{
    memset(info, 0, sizeof(*info));
    return sceKernelVirtualQuery((const void *)addr, 1, info, sizeof(*info)) == 0;
}

void *ps4MapFixed(uintptr_t addr, size_t len, int fd)
{
    Ps4VQInfo info;
    if (ps4NextMapping(addr, &info) && (uintptr_t)info.start < addr + len) {
        ps4Log("[ps4] map %#lx+%#lx: range in use by %p-%p \"%.32s\"", (unsigned long)addr,
               (unsigned long)len, info.start, info.end, info.name);
        return NULL;
    }
    const int flags = (fd < 0 ? (MAP_PRIVATE | MAP_ANON) : MAP_SHARED) | MAP_FIXED;
    void *p = __real_mmap((void *)addr, len, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (p != (void *)addr) {
        ps4Log("[ps4] map %#lx+%#lx failed: %p (errno %d)", (unsigned long)addr,
               (unsigned long)len, p, p == MAP_FAILED ? errno : 0);
        if (p != MAP_FAILED) munmap(p, len);
        return NULL;
    }
    return p;
}

/* Thread stacks: bump-allocated in [0x30000000, 0x40000000), below 2 GiB and
 * clear of the ranges fast3d decodes (cart map < 0x20000000, the D131 module
 * range from 0x40000000, DRAM from 0x70000000). A 16 KiB gap between stacks
 * stays unmapped as a guard. */
#define PS4_STACK_BASE 0x30000000UL
#define PS4_STACK_END  0x40000000UL
#define PS4_PAGE       0x4000UL

void *ps4AllocLowStack(size_t size)
{
    static uintptr_t next = PS4_STACK_BASE;
    size = (size + PS4_PAGE - 1) & ~(PS4_PAGE - 1);
    if (next + size > PS4_STACK_END) {
        ps4Log("[ps4] low stacks exhausted (%#lx + %#lx)", (unsigned long)next,
               (unsigned long)size);
        return NULL;
    }
    void *p = ps4MapFixed(next, size, -1);
    if (p) next += size + PS4_PAGE;
    return p;
}

int ps4SystemUiOverlaid(void)
{
    static uint64_t lastUs;
    static int overlaid, logged;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (lastUs && now - lastUs < 100000u) return overlaid;
    lastUs = now;
    Ps4SystemServiceStatus st;
    memset(&st, 0, sizeof(st));
    int r = sceSystemServiceGetStatus(&st);
    if (r != 0) {
        if (!logged++) ps4Log("[ps4] sceSystemServiceGetStatus: %#x", (unsigned)r);
        return overlaid = 0;
    }
    return overlaid = st.isSystemUiOverlaid || st.isInBackgroundExecution;
}

void ps4Halt(const char *why)
{
    ps4Log("[ps4] halted: %s (close the game from the PS button menu)", why ? why : "");
    for (;;) sleep(60);
}

static void ps4RecordImageRange(void)
{
    /* A read-only object: .rodata comes first after the code and the dynamic
     * linking tables, so it shows where the image's data begins. */
    static const char rodata_probe[] = "GoldenEye 64 rodata";
    uintptr_t lo = (uintptr_t)__text_start, hi = lo, data = (uintptr_t)rodata_probe;
    Ps4VQInfo info;
    while (ps4NextMapping(hi, &info) && strncmp(info.name, "executable", 10) == 0) {
        hi = (uintptr_t)info.end;
    }
    ps4Log("[ps4] image %#lx-%#lx, data from %#lx", (unsigned long)lo, (unsigned long)hi,
           (unsigned long)data);
    /* fast3d's seg_addr: display-list values below 0x800000 are N64 RAM offsets
     * and from 0x01000000 (segment number set) segment addresses, so whatever
     * a display list can point at in the image must lie in between (the link
     * script puts .rodata at 0x800000, port/ps4/ps4.cmake). */
    if (data < 0x800000UL || hi > 0x1000000UL) {
        ps4Log("[ps4] WARNING: image data outside 0x800000-0x1000000; display lists will break");
    }
}

static void ps4LoadModules(void)
{
    static const struct { uint32_t id; const char *name; } mods[] = {
        {0x80000010, "SystemService"}, {0x80000011, "UserService"},
        {0x80000001, "AudioOut"},      {0x80000024, "Pad"},
        {0x80000022, "VideoOut"},
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        int r = sceSysmoduleLoadModuleInternal(mods[i].id);
        ps4Log("[ps4] module %s: %#x", mods[i].name, (unsigned)r);
    }
}

/* Read a whole small text file (NUL-terminated, malloc'd), or NULL. */
static char *ps4ReadText(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *buf = malloc(8192);
    ssize_t n = buf ? read(fd, buf, 8191) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    return buf;
}

static char *ps4Trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static void ps4ApplyEnvFile(void)
{
    char *text = ps4ReadText(PS4_DATA_DIR "/env.txt");
    if (!text) return;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        line = ps4Trim(line);
        char *eq = strchr(line, '=');
        if (!*line || *line == '#' || !eq) continue;
        *eq = 0;
        setenv(ps4Trim(line), ps4Trim(eq + 1), 1);
        ps4Log("[ps4] env %s=%s", ps4Trim(line), ps4Trim(eq + 1));
    }
    free(text);
}

static void ps4ApplyArgsFile(int *argc, char ***argv)
{
    static char *args[32];
    char *text = ps4ReadText(PS4_DATA_DIR "/args.txt");
    if (!text) return;   /* kept for the life of the process: argv points into it */
    int n = 0;
    args[n++] = (*argc > 0 && *argv) ? (*argv)[0] : "eboot.bin";
    for (char *line = strtok(text, "\n"); line && n < 31; line = strtok(NULL, "\n")) {
        line = ps4Trim(line);
        if (!*line || *line == '#') continue;
        args[n++] = line;
        ps4Log("[ps4] arg %s", line);
    }
    args[n] = NULL;
    *argc = n;
    *argv = args;
}

void ps4PlatformInit(int *argc, char ***argv)
{
    mkdir(PS4_DATA_DIR, 0777);
    mkdir(PS4_DATA_DIR "/data", 0777);
    s_logFd = open(PS4_DATA_DIR "/ge007.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    ps4Log("[ps4] GoldenEye 007 (N64 decomp PC port) for PS4 starting");
    ps4RecordImageRange();
    ps4LoadModules();
    ps4ApplyEnvFile();
    ps4ApplyArgsFile(argc, argv);
}
