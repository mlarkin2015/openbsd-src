#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		x86_mmu_test
SRCS=		x86_mmu_test.c x86_mmu.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
NOMAN=

REGRESS_TARGETS=	run-regress-x86_mmu
run-regress-x86_mmu: x86_mmu_test
	./x86_mmu_test

.include <bsd.regress.mk>
