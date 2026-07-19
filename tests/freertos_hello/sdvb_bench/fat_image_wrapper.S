/* Embed the bare FAT16 image; the linker places .fat_image at 0x83000000,
 * which is what fat16.h's FAT_PART_BASE points at. */
.section .fat_image, "a", @progbits
.global _fat_image_start
.global _fat_image_end
.align 8

_fat_image_start:
.incbin "sdvb_bench/my_fat_image_sim.img"
_fat_image_end:
