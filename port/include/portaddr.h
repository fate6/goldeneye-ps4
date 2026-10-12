#ifndef PORT_PORTADDR_H
#define PORT_PORTADDR_H

/* Stage B step 1 (STAGEB-ABI-DESIGN, D441 follow-on): the ONE place that
 * names the N64-DRAM arena windows. port/src/dram.c maps an 8 MB backing
 * store at BOTH views; everything that converts or range-checks an arena
 * address uses these instead of a literal, so moving the arena later (the
 * high-arena test flavour, macOS/ARM port_addr) is a change in one header.
 * Compile-time constants: no codegen change from the old literals. */
/* Each define is #ifndef-guarded so a -D on the command line can override it
 * (the D441 high-arena test flavour: GE_HIGHARENA_BASE=0x90000000 adds
 * -DPORT_DRAM_V1_BASE=0x90000000 for C, CXX and ASM). Values unchanged. */
#ifndef PORT_DRAM_V1_BASE
#define PORT_DRAM_V1_BASE  0x70000000UL  /* s32-safe "virtual" view (game RAM symbols) */
#endif
#ifndef PORT_DRAM_K0_BASE
#define PORT_DRAM_K0_BASE  0x80000000UL  /* KSEG0 mirror of V1 (same pages) */
#endif
#ifndef PORT_DRAM_SIZE
#define PORT_DRAM_SIZE     0x00800000UL  /* 8 MB, like an 8-MB-RAM N64 */
#endif

/* PS4: the console loads the (PIE) eboot at 0x400000, so the image's own
 * .data/.rodata sit below PORT_DRAM_SIZE -- the range fast3d decodes as
 * K0-physical DRAM offsets. A truncated pointer to a compiled array (e.g.
 * osVirtualToPhysical(&symbol) in a display list) must stay a host pointer.
 * The image (0x400000 .. ~0x1000000) overlaps only DRAM offsets display lists
 * never use (mempools end at 0x2F4400); ps4PlatformInit checks the bounds.
 * The Linux/Windows images sit far above 0x800000. */
#if !defined(__ASSEMBLER__)
#if defined(__PS4__)
extern unsigned long g_portImageLo, g_portImageHi;
#define PORT_IS_IMAGE_ADDR(a) \
    ((unsigned long)(a) >= g_portImageLo && (unsigned long)(a) < g_portImageHi)
#else
#define PORT_IS_IMAGE_ADDR(a) 0
#endif
#endif /* !__ASSEMBLER__ */

#endif /* PORT_PORTADDR_H */
