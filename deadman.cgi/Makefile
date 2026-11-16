PROG=	deadman.cgi
MAN=	deadman.cgi.8
BINDIR=	/var/www/cgi-bin
MANDIR=	/usr/local/man/man

# slowcgi(8) runs it in httpd(8)'s chroot, which has no shared libraries.
LDSTATIC=	${STATIC}

CFLAGS+=	-I${.CURDIR}/..
CFLAGS+=	-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=	-Wsign-compare

# An install into DESTDIR, as for a package, makes the directories it
# installs into.
.if !empty(DESTDIR)
beforeinstall:
	${INSTALL} -d ${DESTDIR}${BINDIR} ${DESTDIR}${MANDIR}8
.endif

.include <bsd.prog.mk>
