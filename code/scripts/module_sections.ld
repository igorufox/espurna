  /* PROTOTYPE (docs/modules.md): the part of every chip's module link script (scripts/<chip>_module.ld, its SECTIONS placeholder)
     after its own sections: what a module drops (its destructors never run, no exceptions) and the sections that
     take no memory. A chip without some of them simply has no such input sections. */

  /DISCARD/ :
  {
    *(.dtors .dtors.* .fini_array .fini_array.* .text.exit.* .literal.exit.*)
    *(.ARM.exidx .ARM.exidx.* .ARM.extab .ARM.extab.* .gnu.linkonce.armexidx.* .gnu.linkonce.armextab.*)
    *(.eh_frame .eh_frame_hdr .gcc_except_table .gcc_except_table.*)
    *(.comment .note .note.*)
  }

  .ARM.attributes 0 : { KEEP(*(.ARM.attributes)) }
  .xtensa.info 0 : { *(.xtensa.info) }
  .xt.prop 0 : { KEEP(*(.xt.prop .xt.prop.* .gnu.linkonce.prop.*)) }
  .xt.lit 0 : { KEEP(*(.xt.lit .xt.lit.* .gnu.linkonce.p.*)) }
  .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
  .debug_abbrev 0 : { *(.debug_abbrev) }
  .debug_line 0 : { *(.debug_line) }
  .debug_frame 0 : { *(.debug_frame) }
  .debug_str 0 : { *(.debug_str) }
  .debug_line_str 0 : { *(.debug_line_str) }
  .debug_loc 0 : { *(.debug_loc) }
  .debug_loclists 0 : { *(.debug_loclists) }
  .debug_rnglists 0 : { *(.debug_rnglists) }
  .debug_ranges 0 : { *(.debug_ranges) }
  .debug_aranges 0 : { *(.debug_aranges) }
