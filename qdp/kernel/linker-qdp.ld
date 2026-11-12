/* qdp/kernel/linker-qdp.ld - the QEMU DOS port's link: kernel/linker.ld's, with QEMU's globals together.
 *
 * QEMU 0.14 was made to run once, in a process of its own; the QEMU DOS port runs machine after machine in one kernel.
 * So QEMU's own globals - the .data and .bss of build-qdp/qemu/ (QEMU, its glue, zlib, the OPL3 device) - are put in
 * one stretch each, between __qemu_data_start/_end and __qemu_bss_start/_end: before each machine's run kernel/vpc.c
 * puts them back as they were when the kernel started (.data from a copy made then, .bss zeroed), and QEMU's
 * constructors (.init_array: its devices' and block drivers' registrations) run again.  Everything else is as
 * kernel/linker.ld (DOS95's own link, not changed). */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
SECTIONS
{
    . = 0xC0000000;
    .text : { *(.text.entry) *(.text*) }
    /* .rodata is mapped user-readable for ring-3 native programs (fonts, icons,
     * palettes, constants), so it starts on a page of its own and the exposure is
     * exactly this section and nothing else. */
    . = ALIGN(4096);
    .rodata : { __rodata_start = .; *(.rodata*) . = ALIGN(4096); __rodata_end = .; }
    . = ALIGN(4096);
    .data : { __data_start = .;
              . = ALIGN(16); __qemu_data_start = .; build-qdp/qemu/*(.data*) . = ALIGN(16); __qemu_data_end = .;
              *(.data*) }
    .init_array : { __init_array_start = .; KEEP(*(.init_array*)) KEEP(*(.ctors*)) __init_array_end = .; }
    . = ALIGN(4096);
    __bss_start = .;
    .bss : { . = ALIGN(16); __qemu_bss_start = .; build-qdp/qemu/*(.bss*) build-qdp/qemu/*(COMMON) . = ALIGN(16); __qemu_bss_end = .;
             *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    _end = .;
    /* v44 (item 3a, #526): each processor's own data (kernel.h PERCPU) - not in the image: a region whose pages are
     * each CPU's own (start.asm maps the first CPU's, smp.c the others'), zeroed */
    .percpu 0xCF800000 (NOLOAD) : { __percpu_start = .; *(.percpu.user) . = ALIGN(4096); *(.percpu) . = ALIGN(4096); __percpu_end = .; }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
