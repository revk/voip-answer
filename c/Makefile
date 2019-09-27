# Artifacts:

bin/voip-answer: src/voip-answer.c deps/siptools.c build/sip_parsers.o Makefile
	cc -O -o $@ $< build/sip_parsers.o -D_GNU_SOURCE -g -Wall -funsigned-char -lpopt

# Library files:

build/sip_parsers.o: src/sip_parsers.c src/sip_parsers.h Makefile
	cc -o $@ -c $<

# Dependencies

deps/siptools.c: Makefile
	svn export --force -r 28638 svn://changeless.aa.net.uk/fb6000/trunk/apps/siptools.c $@
	touch $@  # SVN sets the timestamp to the committed one (which triggers "building" every time)

# Tests:

bin/test_sip_parsers: test/test_sip_parsers.c build/sip_parsers.o
	cc -o $@ $< build/sip_parsers.o

test: bin/test_sip_parsers
	bin/test_sip_parsers
