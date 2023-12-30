all: pucrunch

.PHONY: clean

pucrunch: pucrunch.c pucrunch.h
	gcc -Wall -Wno-pointer-sign -funsigned-char pucrunch.c -o pucrunch -O3 -lm -Dstricmp=strcasecmp

clean:
	rm -f pucrunch
