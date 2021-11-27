CFLAGS=-DHAVE_MEMRCHR -Os -g -flto -Wall -Wextra -Wwrite-strings
LDFLAGS=-flto
LDFLAGS+=-lncurses

te: te.c libtext.a

libtext.a: vis/array.o vis/text.o vis/text-io.o vis/text-util.o vis/text-motions.o vis/text-iterator.o vis/text-regex.o vis/text-common.o vis/text-objects.o
	$(AR) $(ARFLAGS) $@ $^

clean:
	-rm -f te *.o vis/*.o
