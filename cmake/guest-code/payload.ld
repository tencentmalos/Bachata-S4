/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small rebased payload, no CRT/loader/TLS/global initialization contract yet. */
ENTRY(guest_entry)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}
