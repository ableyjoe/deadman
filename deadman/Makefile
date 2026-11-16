PROG=	deadman
MAN=	deadman.8 deadman.conf.5
BINDIR=	/usr/local/sbin
MANDIR=	/usr/local/man/man

CFLAGS+=	-I${.CURDIR}/..
CFLAGS+=	-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=	-Wsign-compare

beforeinstall:
	@id _deadman >/dev/null 2>&1 || { \
		echo "There is no _deadman user: see README.md."; exit 1; }

afterinstall:
	${INSTALL} -d -o _deadman -g www -m 0710 ${DESTDIR}/var/www/deadman
	${INSTALL} -d -o _deadman -g _deadman -m 0700 ${DESTDIR}/var/db/deadman
	@if [ ! -f ${DESTDIR}/etc/deadman.conf ]; then \
		echo "${INSTALL} -o root -g wheel -m 0600 deadman.conf ${DESTDIR}/etc"; \
		${INSTALL} -o root -g wheel -m 0600 ${.CURDIR}/../deadman.conf \
		    ${DESTDIR}/etc/deadman.conf; \
	fi

.include <bsd.prog.mk>
