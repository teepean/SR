; Windows: the area where lithtech.exe's image goes (0x400000, Primal Hunt's up to 0x500000) is part of the program's own
; image, so that the loader keeps it free (Wine and Windows put heaps and mapped files at low addresses before the
; program runs). The program is linked at 0x3f0000 with this section first (SConstruct: pc64-windows.ld): 0x3f1000 -
; 0x510000. peimage.c copies lithtech.exe into it. Empty in other formats.

%ifidn __OUTPUT_FORMAT__, win64
section .lithimg bss align=4096
global lithimg_area
global lithimg_area_end
lithimg_area:
    resb 0x11F000
lithimg_area_end:
%endif
