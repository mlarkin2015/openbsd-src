#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		smbios_test
SRCS=		smbios_test.c smbios.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
NOMAN=

REGRESS_TARGETS=	run-regress-smbios
run-regress-smbios: smbios_test
	./smbios_test

.include <bsd.regress.mk>
