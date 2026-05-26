OUTPUT_ARCH( "riscv" )
ENTRY( _start )



SECTIONS
{
	. = 0x80000000;
	.init :
	{
		_text = .;
		KEEP (*(SORT_NONE(.init)))
	} 
	. = ALIGN(16);
	.text :
	{
		*(.text.unlikely .text.unlikely.*)
		*(.text.startup .text.startup.*)
		*(.text .text.*)
		*(.gnu.linkonce.t.*)
	} 
	. = ALIGN(16);
	.fini :
	{
		KEEP (*(SORT_NONE(.fini)))
		_etext = .;
	} 
	. = ALIGN(16);
	.rodata.align :
	{
		. = ALIGN(8);
		_rodata = .;
	} 
	. = ALIGN(16);
	.rodata.start :
	{
		_rodata_lma = LOADADDR(.rodata.start);
	} 
	. = ALIGN(16);
	.rodata :
	{
		*(.rdata)
		*(.rodata .rodata.*)
		*(.gnu.linkonce.r.*)

		. = ALIGN(8);
		_erodata = .;
	} 
	. = ALIGN(16);
	.data.align :
	{
		. = ALIGN(8);
		_data = .;
	} 
	. = ALIGN(16);
	.data.start :
	{
		_data_lma = LOADADDR(.data.start);
	} 
	. = ALIGN(16);
	.data :
	{
		*(.data .data.*)
		*(.gnu.linkonce.d.*)
		. = ALIGN(8);
		PROVIDE( __global_pointer$ = . + 0x800 );
		*(.sdata .sdata.*)
		*(.sdata2 .sdata2.*)
		*(.gnu.linkonce.s.*)
		. = ALIGN(8);
		*(.srodata.cst16)
		*(.srodata.cst8)
		*(.srodata.cst4)
		*(.srodata.cst2)
		*(.srodata .srodata.*)

		. = ALIGN(8);
		_edata = .;
	} 
	. = ALIGN(16);
	.bss.align :
	{
		. = ALIGN(8);
		_bss = .;
	} 
	. = ALIGN(16);
	.bss.start :
	{
		_bss_lma = LOADADDR(.bss.start);
	} 
	. = ALIGN(16);
	.bss :
	{
		*(.sbss*)
		*(.gnu.linkonce.sb.*)
		*(.bss .bss.*)
		*(.gnu.linkonce.b.*)
		*(COMMON)

		. = ALIGN(8);
		_ebss = .;
	} 
	. = ALIGN(16);
	. = ALIGN(8);
	_end = .;
	. = ALIGN(16);
	.stack :
	{
		. = ALIGN(16);
		. += 4 * 256K;        /* 4 harts x 256 KiB */
		_stack_top = .;
	}
}
