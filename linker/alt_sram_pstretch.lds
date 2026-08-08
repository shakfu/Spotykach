/* SRAM-execution variant of alt_sram.lds for the ENGINE=pstretch build. Same as alt_sram.lds but with
 * ONE change: the SRAM_EXEC/SRAM split moves from 300K/212K to 200K/312K. Nothing else differs - same
 * origins, same DTCMRAM/RAM_D2/SDRAM/QSPI regions, same _estack.
 *
 * Why pstretch needs its own split. The 512K AXI SRAM at 0x24000000 is partitioned by these two lines
 * into code (SRAM_EXEC) and data (SRAM). Every other engine is comfortable at 300K/212K. pstretch is
 * the one engine that is large in BOTH halves at once, so it is squeezed from both sides:
 *
 *   - data: its FFT working set is 297664 B of fixed-size arrays (_re/_im/_cos/_sin/_brev/_window,
 *     all dimensioned by the compile-time kWindow = PSTRETCH_WINDOW, default 8192). None of it is
 *     dynamic - the size is fixed at compile time, so the region either fits it or the link fails.
 *   - code: ~164 KB on its own, ~193 KB with SPK_TERMINAL linking the USB-device CDC stack.
 *
 * That leaves only a narrow band of split points that satisfies both. Measured with TERMINAL=1:
 *
 *     186K -> SRAM_EXEC overflows by 2836 B
 *     200K -> links (SRAM_EXEC 94.38%, SRAM 97.40%)      <- this script
 *     210K -> SRAM overflows by 1920 B
 *     300K -> SRAM overflows by 13504 B                  <- alt_sram.lds, the shipping default
 *
 * History: alt_sram.lds was 186K/326K until 993210f moved it to 300K/212K so the terminal channel
 * would fit on every engine. pstretch was the engine living in the 114K that move reclaimed, and it
 * stopped linking entirely - terminal or not. Rather than pull the global split back (which would put
 * the terminal's universal fit at risk for the other 20 engines, all of which are happy at 300K), the
 * change is isolated to the one engine that needs it. Same reasoning, and the same precedent, as
 * alt_qspi_chuck.lds.
 *
 * Headroom is genuinely tight here (94% / 97% with the terminal on). If pstretch grows in either
 * direction, the lever is PSTRETCH_WINDOW: `make ENGINE=pstretch WINDOW=4096` drops the data side by
 * 49152 B (exactly half of each kWindow-sized array) at the cost of a shorter, snappier smear.
 *
 * HARDWARE-VERIFIED 2026-08-01, all three images flashed and run on the unit:
 *
 *   WINDOW=8192 TERMINAL=off   SRAM_EXEC 80.24% / SRAM 93.17%   boots, audio correct
 *   WINDOW=8192 TERMINAL=1     SRAM_EXEC 94.38% / SRAM 97.40%   boots, audio correct, channel up,
 *                                                               make test-hw 30 passed
 *   WINDOW=4096 TERMINAL=1     SRAM_EXEC 94.35% / SRAM 82.01%   boots, audio correct, channel up,
 *                                                               make test-hw 30 passed
 *
 * This matters more than a link check: moving the code/data boundary is exactly the kind of change
 * that links clean and then fails at startup, so the split is now established as sound on real
 * silicon rather than arithmetically. Note the 97.40% image runs fine - the tightness is a headroom
 * concern for future growth, not a present defect.
 *
 * CPU, measured over the terminal channel the same day: 8192 runs ~41.5% avg with peaks still rising
 * through the sample (87.6 -> 91.3%, not converged); 4096 runs ~33.3% avg with a converged ~63.6%
 * peak. See docs/engines/pstretch.md for the full table and what the non-convergence implies.
 */

ENTRY(Reset_Handler)

MEMORY
{
	FLASH		(RX)  : ORIGIN = 0x08000000, LENGTH = 128K
	DTCMRAM		(RWX) : ORIGIN = 0x20000000, LENGTH = 128K
	/* The H750's 512K AXI SRAM, split between executable code and data. THE ONLY LINES THAT DIFFER
	 * FROM alt_sram.lds: 200K/312K here, 300K/212K there. See the header for why.
	 *
	 * The hardware caveat from alt_sram.lds still applies and is easier to satisfy here: what has
	 * been flash-verified is that the bootloader copies an image of >= 215K, NOT that it copies a
	 * full 300K. This script's 200K ceiling sits below that tested point, so a pstretch image that
	 * fills it stays inside what the bootloader is known to handle.
	 */
	SRAM_EXEC	(RWX) : ORIGIN = 0x24000000, LENGTH = 200K
	SRAM		(RWX) : ORIGIN = 0x24000000 + 200K, LENGTH = 512K - 200K
	RAM_D2		(RWX) : ORIGIN = 0x30000000, LENGTH = 32K
	RAM_D2CACHE	(RWX) : ORIGIN = 0x30000000 + 32K, LENGTH = 288K - 32K
	RAM_D3		(RWX) : ORIGIN = 0x38000000, LENGTH = 64K
	BACKUP_SRAM	(RWX) : ORIGIN = 0x38800000, LENGTH = 4K
	ITCMRAM		(RWX) : ORIGIN = 0x00000000, LENGTH = 64K
	SDRAM		(RWX) : ORIGIN = 0xc0000000, LENGTH = 64M
	QSPIFLASH	(RX)  : ORIGIN = 0x90000000, LENGTH = 8M
}

_estack = 0x20020000;

SECTIONS
{
	.isr_vector :
	{
		. = ALIGN(4);
		KEEP(*(.isr_vector))
		. = ALIGN(4);
	} > SRAM_EXEC

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

	} > SRAM_EXEC

	.ARM.extab :
	{
		. = ALIGN(4);
		*(.ARM.extab)
		*(.gnu.linkonce.armextab.*)
		. = ALIGN(4);
	} > SRAM_EXEC

	.exidx :
	{
		. = ALIGN(4);
		PROVIDE(__exidx_start = .);
		*(.ARM.exidx*)
		. = ALIGN(4);
		PROVIDE(__exidx_end = .);
	} > SRAM_EXEC

	.ARM.attributes :
	{
		*(.ARM.attributes)
	} > SRAM_EXEC

	.preinit_array :
	{
		PROVIDE(__preinit_array_start = .);
		KEEP(*(.preinit_array*))
		PROVIDE(__preinit_array_end = .);
	} > SRAM_EXEC

	.init_array :
	{
		PROVIDE(__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array*))
		PROVIDE(__init_array_end = .);
	} > SRAM_EXEC

	.fini_array :
	{
		PROVIDE(__fini_array_start = .);
		KEEP(*(.fini_array*))
		KEEP(*(SORT(.fini_array.*)))
		PROVIDE(__fini_array_end = .);
	} > SRAM_EXEC

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
	} > DTCMRAM AT > SRAM_EXEC

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
