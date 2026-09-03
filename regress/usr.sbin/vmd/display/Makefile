#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		display_test
SRCS=		display_test.c display.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
NOMAN=

REGRESS_TARGETS=	run-regress-display
run-regress-display: display_test
	./display_test

.include <bsd.regress.mk>
