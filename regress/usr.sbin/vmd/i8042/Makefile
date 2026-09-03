#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		i8042_test
SRCS=		i8042_test.c i8042.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
LDADD+=		-lpthread
DPADD+=		${LIBPTHREAD}
NOMAN=

REGRESS_TARGETS=	run-regress-i8042
run-regress-i8042: i8042_test
	./i8042_test

.include <bsd.regress.mk>
