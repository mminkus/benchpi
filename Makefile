LVGL_VER := 9.5.0

CC     := gcc
CFLAGS := -O2 -g -Wall -I. -Ilvgl
LDLIBS := -lm

# Expanded at parse time, so `make lvgl` has to run before `make`. See README.
SRCS := src/main.c $(shell find lvgl/src -name '*.c' 2>/dev/null)
OBJS := $(SRCS:.c=.o)

benchpi: $(OBJS)
	$(CC) $(OBJS) $(LDLIBS) -o $@

lvgl:
	curl -sL https://github.com/lvgl/lvgl/archive/refs/tags/v$(LVGL_VER).tar.gz | tar xz
	mv lvgl-$(LVGL_VER) lvgl

clean:
	rm -f $(OBJS) benchpi

.PHONY: clean
