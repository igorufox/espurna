/* PROTOTYPE (docs/esp8266-modules.md): link of one ESP8266 module against the head (--just-symbols).
   scripts/esp8266_module_link.py replaces @FLASH@ / @RAM@ / @IRAM@ with placeholder bases and links twice; the words
   that differ between the two links are the ones the head relocates when it places the module.

   Flash: the header, code, PSTR / PROGMEM, vtables, the ctors table, then the load images of IRAM and RAM.
   RAM: .data and .rodata (byte access, not possible in flash), then .bss. */

MEMORY
{
  mflash (rx) : org = @FLASH@, len = 0x80000
  mram (rw)   : org = @RAM@, len = 0x4000
  miram (rx)  : org = @IRAM@, len = 0x2000
}

SECTIONS
{
  .mod_flash : ALIGN(4)
  {
    __mod_flash_start = ABSOLUTE(.);
    KEEP(*(.module_header))
    . = ALIGN(4);
    *(.literal .text .literal.* .text.* .stub)
    *(.irom0.literal .irom.literal .irom.text.literal .irom0.text .irom0.text.* .irom.text .irom.text.*)
    *(.irom0.pstr.*)
    *(.rodata._ZZ*__pstr__*)
    *(.rodata._ZZ*__FUNCTION__ .rodata._ZZ*__PRETTY_FUNCTION__ .rodata._ZZ*__func__)
    *(.rodata._ZTV*)
    . = ALIGN(4);
    __mod_init_start = ABSOLUTE(.);
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __mod_init_end = ABSOLUTE(.);
    . = ALIGN(4);
  } > mflash

  .mod_iram : ALIGN(4)
  {
    __mod_iram_start = ABSOLUTE(.);
    *(.iram.literal .iram.literal.* .iram.text .iram.text.* .iram0.literal .iram0.text .iram0.text.*)
    . = ALIGN(4);
    __mod_iram_end = ABSOLUTE(.);
  } > miram AT > mflash
  __mod_iram_load = LOADADDR(.mod_iram);

  .mod_data : ALIGN(4)
  {
    __mod_data_start = ABSOLUTE(.);
    *(.data .data.* .gnu.linkonce.d.*)
    *(.rodata .rodata.* .gnu.linkonce.r.* .sdata2 .sdata2.*)
    . = ALIGN(4);
    __mod_data_end = ABSOLUTE(.);
  } > mram AT > mflash
  __mod_data_load = LOADADDR(.mod_data);
  __mod_flash_end = LOADADDR(.mod_data) + SIZEOF(.mod_data);

  .mod_bss (NOLOAD) : ALIGN(4)
  {
    __mod_bss_start = ABSOLUTE(.);
    *(.bss .bss.* .sbss .sbss.* COMMON .gnu.linkonce.b.*)
    . = ALIGN(4);
    __mod_bss_end = ABSOLUTE(.);
  } > mram

  /DISCARD/ :
  {
    *(.dtors .dtors.* .fini_array .fini_array.* .text.exit.* .literal.exit.*)
    *(.eh_frame .gcc_except_table .gcc_except_table.*)
    *(.comment .note .note.*)
  }

  .xtensa.info 0 : { *(.xtensa.info) }
  .xt.prop 0 : { KEEP(*(.xt.prop .xt.prop.* .gnu.linkonce.prop.*)) }
  .xt.lit 0 : { KEEP(*(.xt.lit .xt.lit.* .gnu.linkonce.p.*)) }
  .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
  .debug_abbrev 0 : { *(.debug_abbrev) }
  .debug_line 0 : { *(.debug_line) }
  .debug_frame 0 : { *(.debug_frame) }
  .debug_str 0 : { *(.debug_str) }
  .debug_loc 0 : { *(.debug_loc) }
  .debug_ranges 0 : { *(.debug_ranges) }
  .debug_aranges 0 : { *(.debug_aranges) }
}
