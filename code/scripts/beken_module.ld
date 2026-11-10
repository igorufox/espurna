/* PROTOTYPE (docs/modules.md, "Beken"): link of one BK7231N module against the head (--just-symbols).
   scripts/beken_module_link.py replaces @FLASH@ / @RAM@ with placeholder bases and links twice; the words that differ
   between the two links are the ones the head relocates when it places the module, the BL / BLX into the head
   (relocation records of -q) are moved by the flash delta.

   One contiguous image in flash, read and run through the flash mapping: the header, code, constants, the init
   array, then the load image of .data. RAM (the head's arena): .data, then .bss.
   The discarded sections and the ones without memory: scripts/module_sections.ld, put in at the SECTIONS placeholder. */

MEMORY
{
  mflash (rx) : org = @FLASH@, len = 0x100000
  mram (rw)   : org = @RAM@, len = 0x10000
}

SECTIONS
{
  .mod_text : ALIGN(4)
  {
    __mod_flash_start = ABSOLUTE(.);
    KEEP(*(.module_header))
    . = ALIGN(4);
    *(.text .text.* .gnu.linkonce.t.* .glue_7 .glue_7t .vfp11_veneer .v4_bx)
    . = ALIGN(4);
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(4);
    __mod_init_start = ABSOLUTE(.);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    __mod_init_end = ABSOLUTE(.);
    . = ALIGN(4);
  } > mflash

  __mod_data_lma = ALIGN(LOADADDR(.mod_text) + SIZEOF(.mod_text), 4);

  .mod_data : AT(__mod_data_lma) ALIGN(8)
  {
    __mod_data_start = ABSOLUTE(.);
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(4);
    __mod_data_end = ABSOLUTE(.);
  } > mram
  __mod_data_load = __mod_data_lma;
  __mod_flash_end = __mod_data_lma + SIZEOF(.mod_data);

  .mod_bss (NOLOAD) : ALIGN(8)
  {
    __mod_bss_start = ABSOLUTE(.);
    *(.bss .bss.* COMMON .gnu.linkonce.b.*)
    . = ALIGN(4);
    __mod_bss_end = ABSOLUTE(.);
  } > mram

  /* no IRAM arena on the Beken */
  __mod_iram_load = 0;
  __mod_iram_start = 0;
  __mod_iram_end = 0;

@SECTIONS@
}
