PROG=	deadman
MAN=	deadman.8 deadman.conf.5
BINDIR=	/usr/local/sbin
MANDIR=	/usr/local/man/man
EXAMPLEDIR=	/usr/local/share/examples/deadman

CFLAGS+=	-I${.CURDIR}/..
CFLAGS+=	-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=	-Wsign-compare

# An install into DESTDIR, as for a package, makes the directories it
# installs into, and leaves the _deadman user, deadman's own directories
# and /etc/deadman.conf to whatever installs the result.
.if empty(DESTDIR)
beforeinstall:
	@id _deadman >/dev/null 2>&1 || { \
		echo "There is no _deadman user: see README.md."; exit 1; }
.else
beforeinstall:
	${INSTALL} -d ${DESTDIR}${BINDIR} ${DESTDIR}${MANDIR}5 \
	    ${DESTDIR}${MANDIR}8
.endif

afterinstall:
	${INSTALL} -d ${DESTDIR}${EXAMPLEDIR}
	${INSTALL} ${INSTALL_COPY} -o ${SHAREOWN} -g ${SHAREGRP} \
	    -m ${SHAREMODE} ${.CURDIR}/../deadman.conf ${DESTDIR}${EXAMPLEDIR}
.if empty(DESTDIR)
	${INSTALL} -d -o _deadman -g www -m 0710 /var/www/deadman
	${INSTALL} -d -o _deadman -g _deadman -m 0700 /var/db/deadman
	@if [ ! -f /etc/deadman.conf ]; then \
		echo "${INSTALL} -o root -g _deadman -m 0640 deadman.conf /etc"; \
		${INSTALL} -o root -g _deadman -m 0640 ${.CURDIR}/../deadman.conf \
		    /etc/deadman.conf; \
	fi
.endif

.include <bsd.prog.mk>
