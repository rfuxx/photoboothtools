GPHOTO2CONFIG=/usr/local/bin/gphoto2-config
LIBRAW_PREFIX=/usr/local/stow/libraw
CFLAGS=-O6 -Wall
CC=gcc
LD=gcc

all: continuousCameraCapture quickJpegGutenPrint

clean:
	$(RM) *~ *.o */*.o */*~ continuousCameraCapture quickJpegGutenPrint

continuousCameraCapture.o: continuousCameraCapture.c
	$(CC) $(CFLAGS) $$($(GPHOTO2CONFIG) --cflags) -I$(LIBRAW_PREFIX)/include -c -o continuousCameraCapture.o continuousCameraCapture.c

continuousCameraCapture: transupp/transupp.o continuousCameraCapture.o
	$(LD) -o continuousCameraCapture continuousCameraCapture.o transupp/transupp.o -lpthread -ljpeg -lexif $$($(GPHOTO2CONFIG) --libs) -L$(LIBRAW_PREFIX)/lib -lraw

quickJpegGutenPrint: transupp/transupp.o quickJpegGutenPrint.o
	$(LD) -o quickJpegGutenPrint quickJpegGutenPrint.o transupp/transupp.o -lpthread -ljpeg -lgutenprint -lcups
