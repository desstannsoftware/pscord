/* Lay the shim out as a flat blob with shim_entry at offset 0, no dynamic
 * sections, so the injector can copy it straight into the target and jump to
 * the start of the mapping. */
ENTRY(shim_entry)
SECTIONS {
    . = 0;
    .text : {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
    }
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
    }
}
