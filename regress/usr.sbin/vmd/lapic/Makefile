#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		lapic_test
SRCS=		lapic_test.c i82489dx.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
LDADD+=		-lpthread
DPADD+=		${LIBPTHREAD}
NOMAN=

REGRESS_TARGETS=	run-regress-lapic
run-regress-lapic: lapic_test
	./lapic_test

.include <bsd.regress.mk>
