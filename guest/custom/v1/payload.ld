/* No guest CRT, dynamic linker, TLS, unwinder, or constructors. */
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0;
  .text : { *(.text .text.*) } :code
  .rodata : { *(.rodata .rodata.*) } :code
  .shad_imports : { KEEP(*(.shad_imports)) } :code
  . = ALIGN(4096);
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.* COMMON) } :data
  /DISCARD/ : { *(.comment .note.*) }
}
