CFLAGS ?= -fdiagnostics-color=always -Iinclude -I/usr/include/libdrm -W -Wall -Wextra -g -O2 -std=c11 -DEGL_EGLEXT_PROTOTYPES
LDFLAGS ?=
LIBS    := -lGLESv2 -lEGL -lgbm

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

all: egl-test

egl-test: egl-test.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	-rm -f *.o
	-rm -f egl-test
