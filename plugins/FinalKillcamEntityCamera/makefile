CC=gcc
TARGET_NAME=FinalKillcamEntityCamera

ifeq ($(OS),Windows_NT)
EXT=dll
CFLAGS=-m32 -Wall -O1 -g -mtune=core2
LFLAGS=-m32 -g -shared -static-libgcc
LIBS=-L.. -lcom_plugin
else
EXT=so
CFLAGS=-m32 -Wall -O1 -g -fvisibility=hidden -mtune=core2
LFLAGS=-m32 -g -shared
LIBS=
endif

all: final_killcam_entity_camera.c
	$(CC) $(CFLAGS) -c *.c
	$(CC) $(LFLAGS) -o $(TARGET_NAME).$(EXT) *.o $(LIBS)

clean:
	rm -f *.o $(TARGET_NAME).$(EXT)
