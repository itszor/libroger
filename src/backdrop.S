/* The desktop's backdrop (desktop/backdrop.png, from desktop/mkbackdrop.py),
 * embedded for desktop.c */
    .section .rodata
    .global roger_backdrop_png
    .global roger_backdrop_png_end
    .balign 4
roger_backdrop_png:
    .incbin "desktop/backdrop.png"
roger_backdrop_png_end:
    .section .note.GNU-stack,"",@progbits
