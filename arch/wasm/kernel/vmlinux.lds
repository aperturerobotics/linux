
SECTIONS
{
 . = 0x10000;
 __init_begin = .;
 .head.text : { KEEP(*(.head.text)) }
 .init.text : {
  _sinittext = .;
  *(.init.text .init.text.*)
  *(.text.startup .text.startup.*)
  _einittext = .;
 }
 .init.data : {
  *(.init.data .init.data.*)
  *(.init.rodata .init.rodata.*)
  . = ALIGN(16);
  __setup_start = .;
  KEEP(*(.init.setup))
  __setup_end = .;
  __initcall_start = .;
  KEEP(*(.initcallearly.init))
  __initcall0_start = .;
  KEEP(*(.initcall0.init))
  KEEP(*(.initcall0s.init))
  __initcall1_start = .;
  KEEP(*(.initcall1.init))
  KEEP(*(.initcall1s.init))
  __initcall2_start = .;
  KEEP(*(.initcall2.init))
  KEEP(*(.initcall2s.init))
  __initcall3_start = .;
  KEEP(*(.initcall3.init))
  KEEP(*(.initcall3s.init))
  __initcall4_start = .;
  KEEP(*(.initcall4.init))
  KEEP(*(.initcall4s.init))
  __initcall5_start = .;
  KEEP(*(.initcall5.init))
  KEEP(*(.initcall5s.init))
  __initcallrootfs_start = .;
  KEEP(*(.initcallrootfs.init))
  KEEP(*(.initcallrootfss.init))
  __initcall6_start = .;
  KEEP(*(.initcall6.init))
  KEEP(*(.initcall6s.init))
  __initcall7_start = .;
  KEEP(*(.initcall7.init))
  KEEP(*(.initcall7s.init))
  __initcall_end = .;
  __con_initcall_start = .;
  KEEP(*(.con_initcall.init))
  __con_initcall_end = .;
  . = ALIGN(4);
  __initramfs_start = .;
  KEEP(*(.init.ramfs))
  . = ALIGN(8);
  KEEP(*(.init.ramfs.info))
 }
 __init_end = .;
 .text : {
  _text = .;
  _stext = .;
  *(.text .text.*)
  *(.ref.text)
  *(.sched.text)
  *(.spinlock.text)
  *(.irqentry.text)
  *(.softirqentry.text)
  *(.noinstr.text)
  *(.cpuidle.text)
  *(.fixup)
  _etext = .;
 }
 _sdata = .;
 .rodata : {
  __start_rodata = .;
  *(.rodata .rodata.*)
  *(.data.rel.ro*)
  . = ALIGN(8);
  __start___param = .;
  KEEP(*(__param))
  __stop___param = .;
  . = ALIGN(8);
  __start___modver = .;
  KEEP(*(__modver))
  __stop___modver = .;
  . = ALIGN(8);
  __start___tracepoints_ptrs = .;
  KEEP(*(__tracepoints_ptrs))
  __stop___tracepoints_ptrs = .;
  *(__tracepoints_strings)
  . = ALIGN(8);
  __start_notes = .;
  KEEP(*(.note.*))
  __stop_notes = .;
  . = ALIGN(8);
  __start___jump_table = .;
  KEEP(*(__jump_table))
  __stop___jump_table = .;
  __end_rodata = .;
 }
 .data : {
  *(.data .data.*)
  *(.ref.data)
  *(.data..shared_aligned)
  CONSTRUCTORS
  . = ALIGN(8);
  __start___dyndbg = .;
  KEEP(*(__dyndbg))
  __stop___dyndbg = .;
  . = ALIGN(8);
  __start___dyndbg_classes = .;
  KEEP(*(__dyndbg_classes))
  __stop___dyndbg_classes = .;
  . = ALIGN(32);
  *(__tracepoints)
  . = ALIGN(8);
  __sched_class_highest = .;
  KEEP(*(__stop_sched_class))
  KEEP(*(__dl_sched_class))
  KEEP(*(__rt_sched_class))
  KEEP(*(__fair_sched_class))
  KEEP(*(__ext_sched_class))
  KEEP(*(__idle_sched_class))
  __sched_class_lowest = .;
 }
 . = ALIGN(((1 << 16) << 1));
 __start_init_stack = .;
 init_thread_union = .;
 init_stack = .;
 .data..init_thread_info : {
  KEEP(*(.data..init_thread_info))
 }
 . = ALIGN(((1 << 16) << 1));
 __end_init_stack = .;
 _edata = .;
 . = ALIGN(16);
 __ex_table : {
  __start___ex_table = .;
  KEEP(*(__ex_table))
  __stop___ex_table = .;
 }
 __bss_start = .;
 .bss : {
  *(.bss .bss.*)
  *(COMMON)
 }
 __bss_stop = .;
 _end = .;
 /DISCARD/ : {
  *(.exit.text .exit.data)
  *(.exitcall.exit)
  *(.discard .discard.*)
  *(.export_symbol)
  *(.modinfo)
  *(.gnu.version*)
 }
}
jiffies = jiffies_64;
