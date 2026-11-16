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

.include <bsd.prog.mk>
