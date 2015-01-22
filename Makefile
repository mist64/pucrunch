all: pucrunch

pucrunch: pucrunch.c pucrunch.h
	gcc -Wall -funsigned-char pucrunch.c -o pucrunch -O -lm -Dstricmp=strcasecmp
