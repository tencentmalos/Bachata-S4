/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small rebased payload, no CRT/loader/TLS/global initialization contract yet.
 * .shad_imports holds read-only 8-byte slots the host fills with guest veneer
 * addresses or constants before the image is published read/execute. */
ENTRY(guest_entry)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .shad_imports : { KEEP(*(.shad_imports)) } :code
    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}
