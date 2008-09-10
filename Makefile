CFLAGS=-O3 -funroll-all-loops
#CFLAGS=-O2# -fno-inline
#CFLAGS=-O0
CFLAGS+=-g
CFLAGS+=-W -Wall

TARGETS=path2pgm-dummy path2pgm-show path2pgm-glitter path2pgm-cairo path2pgm-skia

all: $(TARGETS)

path2pgm-dummy: path2pgm.o path2pgm-dummy.c
	$(CC) $(CFLAGS) -Wno-unused -g -o $@ $^ -lm

path2pgm-show: path2pgm.o path2pgm-show.c
	$(CC) $(CFLAGS) -Wno-unused -g -o $@ $^ -lm

path2pgm-glitter: path2pgm.o path2pgm-glitter.c
	$(CC) $(CFLAGS) -g -o $@ $^ -lm

path2pgm-cairo: path2pgm.o path2pgm-cairo.c
	$(CC) $(CFLAGS) -g `pkg-config --cflags cairo` -o $@ $^ `pkg-config --libs cairo` -lm

SKIA=skia
SKIA_INC+=-I$(SKIA)/include
SKIA_INC+=-I$(SKIA)/include/corecg
SKIA_INC+=-I$(SKIA)/sgl
SKIA_INC+=-I$(SKIA)/picture
SKIA_INC+=-I$(SKIA)/corecg

path2pgm-skia: path2pgm.o path2pgm-skia.c
	$(CXX) $(CFLAGS) $(SKIA_INC) -g  -o $@ $^ -L$(SKIA) -lskia -lpthread -lm

clean:
	$(RM) *.o *~
	$(RM) $(TARGETS)
