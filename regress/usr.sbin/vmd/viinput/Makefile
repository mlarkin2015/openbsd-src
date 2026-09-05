#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		viinput_test
SRCS=		viinput_test.c viinput.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
CFLAGS+=	-Wall -Werror -ffunction-sections -fdata-sections
LDFLAGS+=	-Wl,--gc-sections
LDADD+=		-lpthread
DPADD+=		${LIBPTHREAD}
NOMAN=

REGRESS_TARGETS=	run-regress-viinput
run-regress-viinput: viinput_test
	./viinput_test

.include <bsd.regress.mk>
