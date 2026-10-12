// PS4 build only: padding in .data.rel.ro.
//
// PacBrew create-fself (older than OpenOrbis create-fself 626d440) emits the
// data PT_LOAD twice for some .data.rel.ro sizes, and the console then refuses
// to start the app (CE-41839-5). check-oelf catches it at build time; when a
// code change lands on a bad size, change GE_PS4_RELRO_PAD (port/ps4/ps4.cmake).
#include <cstddef>

#ifndef GE_PS4_RELRO_PAD
#define GE_PS4_RELRO_PAD 0
#endif

#if GE_PS4_RELRO_PAD > 0
// Pointers need relocation, so the array goes to .data.rel.ro.
extern "C" __attribute__((used)) const void *const ge_ps4_relro_pad[GE_PS4_RELRO_PAD / sizeof(void *)] = {
    &ge_ps4_relro_pad};
#endif
