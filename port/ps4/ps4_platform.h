#ifndef PORT_PS4_PLATFORM_H
#define PORT_PS4_PLATFORM_H

/*
 * PS4 platform layer (OpenOrbis toolchain, GoldHEN). See docs/ps4/ for the
 * console facts these rely on (fixed low mappings, data folder, modules).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the game reads and writes: ROM, sidecars, ini, saves, logs. */
#define PS4_DATA_DIR "/data/GoldenEye64"

/* Call first thing in main(): opens the log, loads the system modules,
 * applies <data>/env.txt (KEY=VALUE lines, setenv) and records the image
 * range. Rewrites argc/argv from <data>/args.txt (one argument per line)
 * when that file exists. */
void ps4PlatformInit(int *argc, char ***argv);

/* Append one line to <data>/ge007.log (flushed). */
void ps4LogWrite(const char *line, size_t len);

/* Map len bytes at exactly addr (only over a free range; never clobbers).
 * fd < 0: anonymous private memory, else a MAP_SHARED view of fd.
 * Returns addr, or NULL (logged). */
void *ps4MapFixed(uintptr_t addr, size_t len, int fd);

/* A thread stack below 2 GiB (the game truncates stack pointers to u32). */
void *ps4AllocLowStack(size_t size);

/* Never returns: the console reports a crash when an app exits by itself. */
void ps4Halt(const char *why) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* PORT_PS4_PLATFORM_H */
