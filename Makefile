CFLAGS=-O3 -funroll-all-loops
#CFLAGS=-O2
#CFLAGS=-O0
CFLAGS+=-g
CFLAGS+=-W -Wall

all: path2pgm-dummy path2pgm-glitter path2pgm-cairo


path2pgm-dummy: path2pgm.o path2pgm-dummy.c
	$(CC) $(CFLAGS) -Wno-unused -g -o $@ $^ -lm

path2pgm-glitter: path2pgm.o path2pgm-glitter.c
	$(CC) $(CFLAGS) -g -o $@ $^ -lm

path2pgm-cairo: path2pgm.o path2pgm-cairo.c
	$(CC) $(CFLAGS) -g `pkg-config --cflags cairo` -o $@ $^ `pkg-config --libs cairo` -lm

clean:
	$(RM) *.o *~
	$(RM) path2pgm-dummy
	$(RM) path2pgm-glitter
	$(RM) path2pgm-cairo
