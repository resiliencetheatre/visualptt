CC      = gcc
CFLAGS  = -Wall -O2

# Use pkg-config to get GStreamer and GTK flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0)
GST_LIBS   = $(shell pkg-config --libs gstreamer-1.0)

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0)

# Common sources for both binaries
COMMON_SRC = log.c ini.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

# Program 1: recorder
REC_SRC    = visualptt-tx.c
REC_OBJ    = $(REC_SRC:.c=.o)
REC_TARGET = visualptt-tx

# Program 2: GTK spool player
SPOOL_SRC    = visualptt-rx.c
SPOOL_OBJ    = $(SPOOL_SRC:.c=.o)
SPOOL_TARGET = visualptt-rx

# Default rule: build both
all: $(REC_TARGET) $(SPOOL_TARGET)

# Build PTT TX
$(REC_TARGET): $(REC_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS)

# Build GTK RX
$(SPOOL_TARGET): $(SPOOL_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) $(GTK_LIBS)

# Generic rule to compile .c → .o
%.o: %.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) $(GTK_CFLAGS) -c $< -o $@

clean:
	rm -f $(REC_OBJ) $(SPOOL_OBJ) $(COMMON_OBJ) \
	      $(REC_TARGET) $(SPOOL_TARGET)

# -----------------------------------------------------------
# Install and uninstall
# -----------------------------------------------------------

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

install: $(REC_TARGET) $(SPOOL_TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(REC_TARGET)  $(DESTDIR)$(BINDIR)/
	install -m 0755 $(SPOOL_TARGET) $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(REC_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(SPOOL_TARGET)

.PHONY: all clean install uninstall
