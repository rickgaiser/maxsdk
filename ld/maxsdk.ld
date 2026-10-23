/* Linker script for maxsdk EE programs (lld). */

OUTPUT_ARCH(mips)
ENTRY(__start)

PHDRS {
  text PT_LOAD;
}

SECTIONS {
  .text 0x00100000 : {
    KEEP(*(.text.start))
    *(.text .text.*)
  } :text

  /* VU microcode, kept 16-byte aligned so it can be sent with MPG. */
  .vutext : ALIGN(16) { *(.vutext .vutext.*) }

  .rodata : ALIGN(128) { *(.rodata .rodata.*) *(.rdata) }

  .init_array : ALIGN(16) {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  }

  .data : ALIGN(128) { *(.data .data.*) }

  _gp = ALIGN(128) + 0x7ff0;
  .sdata : ALIGN(128) { *(.lit4) *(.lit8) *(.sdata .sdata.*) }

  /* Packet data: aligned to a cache line, so a write back never touches a
     neighbour. */
  .mx.packets : ALIGN(64) { *(.mx.packets .mx.packets.*) }

  .bss (NOLOAD) : ALIGN(128) {
    __bss_start = .;
    *(.sbss .sbss.*) *(.scommon)
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  _end = .;

  /* Scratchpad: 16 KiB at 0x7000_0000, not loaded, not cleared. */
  .spr 0x70000000 (NOLOAD) : { *(.spr .spr.*) }
  ASSERT(SIZEOF(.spr) <= 0x4000, "scratchpad overflow")

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.comment) *(.pdr) }

  /* The kernel picks the stack at the top of memory when __stack is -1. */
  PROVIDE(__stack = -1);
  PROVIDE(__stack_size = 128 * 1024);
}
