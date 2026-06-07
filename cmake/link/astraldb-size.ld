/* AstralDB Release size linker script (ELF / lld). */
SECTIONS
{
	/DISCARD/ :
	{
		*(.comment)
		*(.note*)
		*(.GCC.command.line)
		*(.gnu.build.attributes)
	}
}
