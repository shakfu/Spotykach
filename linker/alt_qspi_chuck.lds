/* QSPI-execution variant of alt_sram.lds for the ENGINE=chuck build. Same as alt_qspi.lds but with
 * ONE extra change: it reclaims the 186K SRAM_EXEC region into SRAM. In a QSPI build no code lands in
 * SRAM_EXEC (it executes in place from QSPIFLASH), so that 186K of AXI SRAM is free; folding it into
 * SRAM gives data the full 512K AXI SRAM instead of 326K. ChucK needs this: CoreUI + libDaisy .bss
 * (~340K) overflows the 326K SRAM region of the stock alt_qspi.lds. The csound QSPI build keeps the
 * stock alt_qspi.lds (its smaller engine fits 326K), so this isolates the change to the engine that
 * needs it. alt_sram.lds (the shipping SRAM build, where SRAM_EXEC really holds the code) is untouched.
 *
 * Target: STM32H750IB. Provided under the BSD license.
 */

ENTRY(Reset_Handler)

MEMORY
{
	FLASH		(RX)  : ORIGIN = 0x08000000, LENGTH = 128K
	DTCMRAM		(RWX) : ORIGIN = 0x20000000, LENGTH = 128K
	/* SRAM_EXEC is unused in the QSPI build (code executes in place from QSPIFLASH), so SRAM spans the
	 * whole 512K AXI SRAM at 0x24000000. SRAM_EXEC is kept (zero-length) so any section naming it still
	 * resolves. This is the one line that differs from alt_qspi.lds. */
	SRAM_EXEC	(RWX) : ORIGIN = 0x24000000, LENGTH = 0K
	SRAM		(RWX) : ORIGIN = 0x24000000, LENGTH = 512K
	RAM_D2		(RWX) : ORIGIN = 0x30000000, LENGTH = 32K
	RAM_D2CACHE	(RWX) : ORIGIN = 0x30000000 + 32K, LENGTH = 288K - 32K
	RAM_D3		(RWX) : ORIGIN = 0x38000000, LENGTH = 64K
	BACKUP_SRAM	(RWX) : ORIGIN = 0x38800000, LENGTH = 4K
	ITCMRAM		(RWX) : ORIGIN = 0x00000000, LENGTH = 64K
	SDRAM		(RWX) : ORIGIN = 0xc0000000, LENGTH = 64M
	QSPIFLASH	(RX)  : ORIGIN = 0x90040000, LENGTH = 7936K
}

/* Main stack at the TOP of the 512K AXI SRAM (grows down into the ~169K free above .bss), NOT the
 * 128K DTCMRAM (which is ~76% full with .data, leaving only ~31K of stack). ChucK builds its type
 * system and parses recursively and needs far more stack than that - a too-small stack overflows down
 * into .dtcmram_bss and corrupts memory (the layout-dependent boot crashes we saw). AXI SRAM is live
 * out of reset (no FMC needed), so it is valid as the stack from the first instruction. ChucK-only. */
_estack = ORIGIN(SRAM) + LENGTH(SRAM);   /* 0x24080000 */

SECTIONS
{
	.isr_vector :
	{
		. = ALIGN(4);
		KEEP(*(.isr_vector))
		. = ALIGN(4);
	} > QSPIFLASH

	.text :
	{
		. = ALIGN(4);
		_stext = .;

		*(.text)
		*(.text*)
		*(.rodata)
		*(.rodata*)
		*(.glue_7)
		*(.glue_7t)
		KEEP(*(.init))
		KEEP(*(.fini))
		. = ALIGN(4);
		_etext = .;

	} > QSPIFLASH

	.ARM.extab :
	{
		. = ALIGN(4);
		*(.ARM.extab)
		*(.gnu.linkonce.armextab.*)
		. = ALIGN(4);
	} > QSPIFLASH

	.exidx :
	{
		. = ALIGN(4);
		PROVIDE(__exidx_start = .);
		*(.ARM.exidx*)
		. = ALIGN(4);
		PROVIDE(__exidx_end = .);
	} > QSPIFLASH

	.ARM.attributes :
	{
		*(.ARM.attributes)
	} > QSPIFLASH

	.preinit_array :
	{
		PROVIDE(__preinit_array_start = .);
		KEEP(*(.preinit_array*))
		PROVIDE(__preinit_array_end = .);
	} > QSPIFLASH

	.init_array :
	{
		PROVIDE(__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array*))
		PROVIDE(__init_array_end = .);
	} > QSPIFLASH

	.fini_array :
	{
		PROVIDE(__fini_array_start = .);
		KEEP(*(.fini_array*))
		KEEP(*(SORT(.fini_array.*)))
		PROVIDE(__fini_array_end = .);
	} > QSPIFLASH

	.data :
	{
		. = ALIGN(4);
		_sdata = .;

		PROVIDE(__data_start__ = _sdata);
		*(.data)
		*(.data*)
		. = ALIGN(4);
		_edata = .;

		PROVIDE(__data_end__ = _edata);
	} > DTCMRAM AT > QSPIFLASH

	_sidata = LOADADDR(.data);

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sbss = .;

		PROVIDE(__bss_start__ = _sbss);
		*(.bss)
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;

		PROVIDE(__bss_end__ = _ebss);
	} > SRAM

	PROVIDE(end = .);

	.dtcmram_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sdtcmram_bss = .;

		PROVIDE(__dtcmram_bss_start__ = _sdtcmram_bss);
		*(.dtcmram_bss)
		*(.dtcmram_bss*)
		. = ALIGN(4);
		_edtcmram_bss = .;

		PROVIDE(__dtcmram_bss_end__ = _edtcmram_bss);
	} > DTCMRAM

	.sram1_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_ssram1_bss = .;

		PROVIDE(__sram1_bss_start__ = _sram1_bss);
		*(.sram1_bss)
		*(.sram1_bss*)
		. = ALIGN(4);
		_esram1_bss = .;

		PROVIDE(__sram1_bss_end__ = _esram1_bss);
	} > RAM_D2

	.d2_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sd2_bss = .;

		PROVIDE(__d2_bss_start__ = _d2_bss);
		*(.d2_bss)
		*(.d2_bss*)
		. = ALIGN(4);
		_ed2_bss = .;

		PROVIDE(__d2_bss_end__ = _ed2_bss);
	} > RAM_D2CACHE

	.sdram_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_ssdram_bss = .;

		PROVIDE(__sdram_bss_start = _ssdram_bss);
		*(.sdram_bss)
		*(.sdram_bss*)
		. = ALIGN(4);
		_esdram_bss = .;

		PROVIDE(__sdram_bss_end = _esdram_bss);
	} > SDRAM

	.backup_sram (NOLOAD) :
	{
		. = ALIGN(4);
		_sbackup_sram = .;

		PROVIDE(__backup_sram_start = _sbackup_sram);
		*(.backup_sram)
		*(.backup_sram*)
		. = ALIGN(4);
		_ebackup_sram = .;

		PROVIDE(__backup_sram_end = _ebackup_sram);
	} > BACKUP_SRAM

	.qspiflash_text :
	{
		. = ALIGN(4);
		_sqspiflash_text = .;

		PROVIDE(__qspiflash_text_start = _sqspiflash_text);
		*(.qspiflash_text)
		*(.qspiflash_text*)
		. = ALIGN(4);
		_eqspiflash_text = .;

		PROVIDE(__qspiflash_text_end = _eqspiflash_text);
	} > QSPIFLASH

	.qspiflash_data :
	{
		. = ALIGN(4);
		_sqspiflash_data = .;

		PROVIDE(__qspiflash_data_start = _sqspiflash_data);
		*(.qspiflash_data)
		*(.qspiflash_data*)
		. = ALIGN(4);
		_eqspiflash_data = .;

		PROVIDE(__qspiflash_data_end = _eqspiflash_data);
	} > QSPIFLASH

	.qspiflash_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sqspiflash_bss = .;

		PROVIDE(__qspiflash_bss_start = _sqspiflash_bss);
		*(.qspiflash_bss)
		*(.qspiflash_bss*)
		. = ALIGN(4);
		_eqspiflash_bss = .;

		PROVIDE(__qspiflash_bss_end = _eqspiflash_bss);
	} > QSPIFLASH

	.heap (NOLOAD) :
	{
		. = ALIGN(4);
		PROVIDE(__heap_start__ = .);
		KEEP(*(.heap))
		. = ALIGN(4);
		PROVIDE(__heap_end__ = .);
	} > SRAM

	.reserved_for_stack (NOLOAD) :
	{
		. = ALIGN(4);
		PROVIDE(__reserved_for_stack_start__ = .);
		KEEP(*(.reserved_for_stack))
		. = ALIGN(4);
		PROVIDE(__reserved_for_stack_end__ = .);
	} > SRAM

    DISCARD :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

}
