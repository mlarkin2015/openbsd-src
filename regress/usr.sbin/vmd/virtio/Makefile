#	$OpenBSD$

VMD_DIR=	usr.sbin/vmd
.PATH:		${.CURDIR}/../../../../${VMD_DIR}

PROG=		virtio_cfg_test
SRCS=		virtio_cfg_test.c virtio.c
CFLAGS+=	-I${.CURDIR}/../../../../${VMD_DIR}
CFLAGS+=	-Wall -Werror -ffunction-sections -fdata-sections
LDFLAGS+=	-Wl,--gc-sections
NOMAN=

REGRESS_TARGETS=	run-regress-virtio-cfg
run-regress-virtio-cfg: virtio_cfg_test
	./virtio_cfg_test

.include <bsd.regress.mk>
