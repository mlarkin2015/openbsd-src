#	$OpenBSD$

.PATH: ${.CURDIR}/../../../../usr.sbin/vmd

PROG=		ramfb_test
SRCS=		ramfb_test.c ramfb.c
NOMAN=		yes
CFLAGS+=	-I${.CURDIR}/../../../../usr.sbin/vmd
CFLAGS+=	-Wall -Werror

REGRESS_TARGETS=	run-regress-ramfb
run-regress-ramfb: ramfb_test
	./ramfb_test

.include <bsd.regress.mk>
