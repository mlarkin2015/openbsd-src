#	$OpenBSD: Makefile,v 1.3 2021/06/13 21:43:35 dv Exp $

.if ${MACHINE} == "amd64"
SUBDIR =
SUBDIR += config diskfmt display lapic ramfb rfb smbios x86_mmio x86_mmu

.elif make(regress) || make(all)
${.TARGETS}:
	# vmd(8) exists only on amd64, but not on ${MACHINE}
	@echo SKIPPED
.endif

install:

.include <bsd.subdir.mk>
