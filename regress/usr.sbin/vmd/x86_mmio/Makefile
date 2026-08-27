#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		x86_mmio_test
SRCS=		x86_mmio_test.c x86_mmio.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
NOMAN=

REGRESS_TARGETS=	run-regress-x86_mmio
run-regress-x86_mmio: x86_mmio_test
	./x86_mmio_test

.include <bsd.regress.mk>
