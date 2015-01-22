#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned char UBYTE;

static far UBYTE mem[65536];

int main(int argc,char *argv[])
{
    int i=0, low=65535, hi=0;
    FILE *handle;
    UBYTE la[2];

    if(argc<2)
    {
        fprintf(stderr,"Usage: %s [input-file [,load-address] ]*\n",
		argv[0]);
	return 5;
    }
    memset(mem, sizeof(mem), 0);
    for(i=1;i<argc;i++)
    {
	char temp[100];
	UBYTE la[2];
	int t = 0, c;
	int loadaddr = -1;

	while(t<99 && argv[i][t] && argv[i][t]!=',')
	{
	    temp[t] = argv[i][t];
	    t++;
	}
	temp[t] = '\0';
	while(argv[i][t] && argv[i][t]!=',')
	    t++;
	if(argv[i][t])
	{
	    loadaddr = strtol(argv[i]+t+1, NULL, 0);
	}
	handle = fopen(temp, "rb");
	if(!handle)
	{
	    fprintf(stderr, "Could not open %s for reading!\n", temp);
	    break;
	}
	fread(la, sizeof(UBYTE), 2, handle);
	if(loadaddr==-1)
	    loadaddr = la[0] + 256*la[1];
	low = min(low, loadaddr);
	t = loadaddr;
	fprintf(stderr, "Loading %s: 0x%04x..", temp,
		(t & 0xffff));
	fflush(stderr);
	while((c = fgetc(handle)) != EOF)
	{
	    mem[t++ & 0xffff] = c;
	    hi = max(hi, (t & 0xffff));
	}
	fprintf(stderr, "0x%04x\n", (t & 0xffff));
	fclose(handle);
    }
    fprintf(stderr, "Saving from 0x%04x to 0x%04x\n", low, hi);

    la[0] = (low & 0xff);
    la[1] = (low>>8);
    fwrite(la, sizeof(UBYTE), 2, stdout);
    fwrite(mem + low, sizeof(UBYTE), hi-low, stdout);
    return 0;
}


