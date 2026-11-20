/* PROTOTYPE (docs/modules.md, "ESP32"): link of one ESP32 module against the head (--just-symbols).
   scripts/esp32_module_link.py replaces @DROM@ / @IROM@ / @RAM@ with placeholder bases and links twice; the words
   that differ between the two links are the ones the head relocates when it places the module.

   One contiguous image in flash, seen through two MMU views of the same bytes:
     data view (DROM): the header, .rodata (strings, vtables, the init array), later the load images of the pool code
     and of .data
     instruction view (IROM): .literal + .text, at the IROM address of the same offset in the slot
   The pool code (package version 7, @IRAM@): the IRAM_ATTR code (.iram1 ...: what runs with the flash cache off, the
   BT controller's ISR), copied into the IRAM pool of the head (modules_esp32.cpp pools()) when the module starts.
   RAM (the head's arena): .data, then .bss.
   The load addresses (LMA) are all in the data view, so `objcopy -O binary` writes the image as it lies in flash.
   The discarded sections and the ones without memory: scripts/module_sections.ld, put in at the SECTIONS placeholder. */

MEMORY
{
  mdrom (r)  : org = @DROM@, len = 0x300000
  mirom (rx) : org = @IROM@, len = 0x300000
  mram (rw)  : org = @RAM@, len = 0x20000
  miram (rx) : org = @IRAM@, len = 0x20000
}

SECTIONS
{
  .mod_rodata : ALIGN(4)
  {
    __mod_flash_start = ABSOLUTE(.);
    KEEP(*(.module_header))
    . = ALIGN(4);
    *(.rodata .rodata.* .gnu.linkonce.r.* .sdata2 .sdata2.* .srodata .srodata.*)
    *(.flash.rodata .flash.rodata.*)
    . = ALIGN(4);
    __mod_init_start = ABSOLUTE(.);
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __mod_init_end = ABSOLUTE(.);
    . = ALIGN(4);
  } > mdrom

  __mod_text_lma = ALIGN(LOADADDR(.mod_rodata) + SIZEOF(.mod_rodata), 4);

  /* the same offset in the instruction view */
  .mod_text (@IROM@ + (__mod_text_lma - @DROM@)) : AT(__mod_text_lma)
  {
    __mod_text_start = ABSOLUTE(.);
    *(.literal .text .literal.* .text.* .stub .gnu.linkonce.t.* .gnu.linkonce.literal.*)
    *(.flash.text .flash.text.* .flash.literal .flash.literal.*)
    . = ALIGN(4);
    __mod_text_end = ABSOLUTE(.);
  }

  __mod_iram_lma = ALIGN(LOADADDR(.mod_text) + SIZEOF(.mod_text), 4);

  /* the pool code: in the IRAM pool, its bytes in flash behind the code (32-bit copies: whole words) */
  .mod_iram : AT(__mod_iram_lma) ALIGN(4)
  {
    __mod_iram_section = ABSOLUTE(.);
    /* the literals first: an l32r reaches back only (esp32_module_link.py iram_literals()) */
    *(.iram1.*literal .iram1.*literal.* .iram0.literal .iram.literal)
    *(.iram1 .iram1.* .iram0.text .iram.text .iram.text.*)
    . = ALIGN(4);
    __mod_iram_section_end = ABSOLUTE(.);
  } > miram
  /* Header::iram_*: the pool code; none - an empty range at the start of the code (no address of the pool in it) */
  __mod_iram_start = (__mod_iram_section_end > __mod_iram_section) ? __mod_iram_section : __mod_text_start;
  __mod_iram_end = (__mod_iram_section_end > __mod_iram_section) ? __mod_iram_section_end : __mod_text_start;
  __mod_iram_load = __mod_iram_lma;

  __mod_data_lma = ALIGN(LOADADDR(.mod_iram) + SIZEOF(.mod_iram), 4);

  .mod_data : AT(__mod_data_lma) ALIGN(8)
  {
    __mod_data_start = ABSOLUTE(.);
    *(.data .data.* .gnu.linkonce.d.* .sdata .sdata.* .dram1 .dram1.*)
    . = ALIGN(4);
    __mod_data_end = ABSOLUTE(.);
  } > mram
  __mod_data_load = __mod_data_lma;
  __mod_flash_end = __mod_data_lma + SIZEOF(.mod_data);

  .mod_bss (NOLOAD) : ALIGN(8)
  {
    __mod_bss_start = ABSOLUTE(.);
    *(.bss .bss.* .sbss .sbss.* COMMON .gnu.linkonce.b.* .noinit .noinit.* .dram1.bss .dram1.bss.*)
    . = ALIGN(4);
    __mod_bss_end = ABSOLUTE(.);
  } > mram

@SECTIONS@
}
