COMMENT =	send mail when jobs stop checking in

V =		1.1
DISTNAME =	deadman-${V}

CATEGORIES =	sysutils

HOMEPAGE =	https://github.com/ableyjoe/deadman

MAINTAINER =	Joe Abley <jabley@strandkip.nl>

# ISC
PERMIT_PACKAGE =	Yes

# uses pledge() and unveil()
WANTLIB =	c

DEBUG_PACKAGES =	${BUILD_PACKAGES}

SITES =		https://github.com/ableyjoe/deadman/releases/download/v${V}/

.include <bsd.port.mk>
