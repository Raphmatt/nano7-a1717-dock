/* Flat image linked at LOAD_ADDR (4 KB of unused space at the end of osos .text in 1.1.2). */
ENTRY(payload_entry)
SECTIONS {
    . = LOAD_ADDR;
    .text : {
        __text_start__ = .;
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
        __tramp = .;            /* 8 B trampoline, written at install time */
        . += 8;
        __text_end__ = .;
    }
    /DISCARD/ : { *(.bss*) *(COMMON) *(.ARM.*) *(.comment) }
}
/* The loader calls LOAD_ADDR|1, so the entry must be the very first byte. */
ASSERT(payload_entry == LOAD_ADDR, "payload_entry is not at LOAD_ADDR (section alignment?)")
ASSERT(__text_end__ - LOAD_ADDR <= 0xF00, "image too large for the free space")
