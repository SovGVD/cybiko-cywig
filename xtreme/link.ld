/* One image for a CyOS .e module: code, read-only data, data and the
 * zeroed data all in one block (CyOS's loader only handles a small bss).
 * tools/cybiko_mkapp.py links this twice at different bases to find the
 * words the loader must relocate. */
OUTPUT_FORMAT("coff-h8300")
OUTPUT_ARCH(h8300s)
INCLUDE xtreme/gate.ld
SECTIONS
{
    .text : {
        *(.text)
        *(.rodata)
        *(.rdata)
        *(.data)
        . = ALIGN(4);
        *(.bss)
        *(COMMON)
        . = ALIGN(4);
        __image_end = .;
    }
    .bss : {
        __bss_start = .;
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.stab) *(.stabstr) }
}
