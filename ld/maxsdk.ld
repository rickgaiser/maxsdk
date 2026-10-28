/* Linker script for maxsdk EE programs (lld). */

OUTPUT_ARCH(mips)
ENTRY(__start)

/* EE main memory below 0x100000 is the kernel's and the loader's (ps2link
   at 0x94000 - 0x100000); the kernel puts the stack at the top of memory
   (__stack -1), the heap is from _end up to it. */
MEMORY {
  ram : ORIGIN = 0x00100000, LENGTH = 0x02000000 - 0x00100000
  spr : ORIGIN = 0x70000000, LENGTH = 16K
}

PHDRS {
  text PT_LOAD;
}

SECTIONS {
  .text 0x00100000 : {
    __text_start = .;
    KEEP(*(.text.start))
    *(.text .text.*)
    __text_end = .;
  } >ram :text

  /* VU microcode, kept 16-byte aligned so it can be sent with MPG. */
  .vutext : ALIGN(16) { *(.vutext .vutext.*) } >ram

  .rodata : ALIGN(128) { *(.rodata .rodata.*) *(.rdata) } >ram

  .init_array : ALIGN(16) {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } >ram

  .data : ALIGN(128) { *(.data .data.*) } >ram

  _gp = ALIGN(128) + 0x7ff0;
  .sdata : ALIGN(128) { *(.lit4) *(.lit8) *(.sdata .sdata.*) } >ram

  /* Packet data: aligned to a cache line, so a write back never touches a
     neighbour. */
  .mx.packets : ALIGN(64) { *(.mx.packets .mx.packets.*) } >ram

  .bss (NOLOAD) : ALIGN(128) {
    __bss_start = .;
    *(.sbss .sbss.*) *(.scommon)
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } >ram
  _end = .;

  /* Scratchpad: 16 KiB at 0x7000_0000, not loaded, not cleared. */
  .spr (NOLOAD) : { *(.spr .spr.*) } >spr

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.comment) *(.pdr) }

  /* The kernel picks the stack at the top of memory when __stack is -1. */
  PROVIDE(__stack = -1);
  PROVIDE(__stack_size = 128 * 1024);
}

ASSERT(_end <= ORIGIN(ram) + LENGTH(ram) - __stack_size,
       "the program leaves no room for the stack at the top of memory")
