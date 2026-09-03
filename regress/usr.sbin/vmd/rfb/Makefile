#	$OpenBSD$

.PATH: ${.CURDIR}/../../../../usr.sbin/vmd

PROG=		rfb_test
SRCS=		rfb_test.c display_worker.c display.c
NOMAN=		yes
CFLAGS+=	-I${.CURDIR}/../../../../usr.sbin/vmd
CFLAGS+=	-DDISPLAY_RFB_TEST -Wall -Werror
LDADD+=		-levent
DPADD+=		${LIBEVENT}

REGRESS_TARGETS=	run-regress-rfb
run-regress-rfb: rfb_test
	./rfb_test

.include <bsd.regress.mk>
