#	deadman: see README.md.
#
#	make && doas make install	build and install both programs
#	make test			run the tests

SUBDIR=	deadman deadman.cgi

test:
	sh ${.CURDIR}/tests/run-tests.sh

.include <bsd.subdir.mk>
