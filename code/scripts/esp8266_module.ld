/* PROTOTYPE (docs/modules.md): link of one ESP8266 module against the head (ESP8266_RTOS_SDK).
   scripts/esp8266_module_link.py replaces @FLASH@ / @RAM@ with placeholder bases and links twice; the words that
   differ between the two links are the ones the head relocates when it places the module.

   Flash (the cache maps it at 0x40200000 + offset): the header, code (IRAM_ATTR code too: no IRAM pool on the RTOS SDK,
   as on ESP32), PSTR / PROGMEM, vtables, the ctors table, then the load image of RAM.
   RAM (the heap block): .data, then .bss. The constants (.rodata) stay in flash as the head's do (owner 2026-11-18):
   a byte read of flash goes through the SDK's LoadStoreError handler, slower, and the modules' RAM is their variables
   only (relay 10.4 KB -> 0.2 KB: almost all of it was strings and tables).
   The discarded sections and the ones without memory: scripts/module_sections.ld, put in at the SECTIONS placeholder. */

/* the hash functions of the ROM (libs/hash.h on the ESP8266): the same addresses as the SDK's esp8266.rom.ld, here for a
   module that calls one the head does not (the head's symbols have only what the head uses) */
PROVIDE ( SHA1Init = 0x4000b584 );
PROVIDE ( SHA1Update = 0x4000b5a8 );
PROVIDE ( SHA1Final = 0x4000b648 );
PROVIDE ( MD5Init = 0x40009818 );
PROVIDE ( MD5Update = 0x40009834 );
PROVIDE ( MD5Final = 0x40009900 );

/* the registers of the peripherals: the same addresses as the SDK's esp8266.peripherals.ld, here for a module whose
   driver the head does not have (the SDK's SPI driver of the spi module: SPI0 / SPI1) */
PROVIDE ( GPIO = 0x60000300 );
PROVIDE ( uart0 = 0x60000000 );
PROVIDE ( uart1 = 0x60000f00 );
PROVIDE ( frc1 = 0x60000600 );
PROVIDE ( rtc_sys_info = 0x60001100 );
PROVIDE ( SLC0 = 0x60000B00 );
PROVIDE ( I2S0 = 0x60000e00 );
PROVIDE ( SPI1 = 0x60000100 );
PROVIDE ( SPI0 = 0x60000200 );

MEMORY
{
  mflash (rx) : org = @FLASH@, len = 0x80000
  /* a bound of the link only (nspanel has more than 16 KB): whether the RAM of a module fits is the board's Apply,
     from its heap */
  mram (rw)   : org = @RAM@, len = 0x10000
}

SECTIONS
{
  .mod_flash : ALIGN(4)
  {
    __mod_flash_start = ABSOLUTE(.);
    KEEP(*(.module_header))
    . = ALIGN(4);
    *(.literal .text .literal.* .text.* .stub)
    *(.iram.literal .iram.literal.* .iram.text .iram.text.* .iram0.literal .iram0.text .iram0.text.* .iram1 .iram1.*)
    *(.irom0.literal .irom.literal .irom.text.literal .irom0.text .irom0.text.* .irom.text .irom.text.*)
    *(.irom0.pstr.*)
    *(.rodata._ZZ*__pstr__*)
    *(.rodata._ZZ*__FUNCTION__ .rodata._ZZ*__PRETTY_FUNCTION__ .rodata._ZZ*__func__)
    *(.rodata._ZTV*)
    /* the constants of every module, mbedtls' tables too (owner 2026-11-18): in flash as the head's; a byte read goes
       through the SDK's LoadStoreError handler (freertos/port/esp8266/xtensa_vectors.S) */
    *(.rodata .rodata.* .gnu.linkonce.r.* .sdata2 .sdata2.*)
    . = ALIGN(4);
    __mod_init_start = ABSOLUTE(.);
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __mod_init_end = ABSOLUTE(.);
    . = ALIGN(4);
  } > mflash

  .mod_data : ALIGN(4)
  {
    __mod_data_start = ABSOLUTE(.);
    *(.data .data.* .gnu.linkonce.d.* .dram0.data .dram0.* .dram1.*)
    . = ALIGN(4);
    __mod_data_end = ABSOLUTE(.);
  } > mram AT > mflash
  __mod_data_load = LOADADDR(.mod_data);
  __mod_flash_end = LOADADDR(.mod_data) + SIZEOF(.mod_data);
  /* no IRAM part (the header has the fields of every chip): as Beken */
  __mod_iram_load = 0;
  __mod_iram_start = 0;
  __mod_iram_end = 0;

  .mod_bss (NOLOAD) : ALIGN(4)
  {
    __mod_bss_start = ABSOLUTE(.);
    *(.bss .bss.* .sbss .sbss.* COMMON .gnu.linkonce.b.*)
    . = ALIGN(4);
    __mod_bss_end = ABSOLUTE(.);
  } > mram

@SECTIONS@
}
