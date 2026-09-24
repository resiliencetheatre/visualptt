CC      = gcc
CFLAGS  = -Wall -O2

# Use pkg-config to get GStreamer and GTK flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-pbutils-1.0)
GST_LIBS   = $(shell pkg-config --libs gstreamer-1.0 gstreamer-pbutils-1.0)

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0)

XMPP_CFLAGS = $(shell pkg-config --cflags libstrophe libcurl openssl)
XMPP_LIBS   = $(shell pkg-config --libs libstrophe libcurl openssl)

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

# Program 3: GTK persistent message-list receiver
LIST_SRC    = visualptt-rx-list.c
LIST_OBJ    = $(LIST_SRC:.c=.o)
LIST_TARGET = visualptt-rx-list

# Program 4: combined GTK receiver and PTT transmitter
COMBINED_SRC    = visualptt.c
COMBINED_OBJ    = $(COMBINED_SRC:.c=.o)
COMBINED_TARGET = visualptt

# Program 5: asynchronous message annotation watcher
WATCHER_SRC    = annotation_watcher.c
WATCHER_OBJ    = $(WATCHER_SRC:.c=.o)
WATCHER_TARGET = annotation-watcher

# Program 6: optional XMPP HTTP Upload exporter
XMPP_SRC    = visualptt-xmpp-send.c
XMPP_OBJ    = $(XMPP_SRC:.c=.o)
XMPP_TARGET = visualptt-xmpp-send

# Default rule: build all programs
all: $(REC_TARGET) $(SPOOL_TARGET) $(LIST_TARGET) $(COMBINED_TARGET) \
	$(WATCHER_TARGET) $(XMPP_TARGET) visualptt-collector

# Build PTT TX
$(REC_TARGET): $(REC_OBJ) $(COMMON_OBJ) recording.o
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS)

# Build GTK RX
$(SPOOL_TARGET): $(SPOOL_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) $(GTK_LIBS)

# Build GTK persistent message-list RX
$(LIST_TARGET): $(LIST_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) $(GTK_LIBS)

# Build combined GTK RX + PTT TX
$(COMBINED_TARGET): $(COMBINED_OBJ) $(COMMON_OBJ) recording.o
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) $(GTK_LIBS)

$(COMBINED_OBJ): visualptt-rx-list.c visualptt-tx.c

# Build annotation watcher (no GTK or GStreamer dependencies)
$(WATCHER_TARGET): $(WATCHER_OBJ) recording.o
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -o $@ $^

$(WATCHER_OBJ): $(WATCHER_SRC)
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -c $< -o $@

$(XMPP_TARGET): $(XMPP_OBJ) ini.o
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -o $@ $^ $(XMPP_LIBS)

$(XMPP_OBJ): $(XMPP_SRC) ini.h
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic $(XMPP_CFLAGS) -c $< -o $@

# Generic rule to compile .c → .o
%.o: %.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) $(GTK_CFLAGS) -c $< -o $@

clean:
	rm -f $(REC_OBJ) $(SPOOL_OBJ) $(LIST_OBJ) $(COMBINED_OBJ) $(WATCHER_OBJ) $(XMPP_OBJ) \
	      $(COMMON_OBJ) recording.o sha256.o $(REC_TARGET) $(SPOOL_TARGET) $(LIST_TARGET) \
	      $(COMBINED_TARGET) $(WATCHER_TARGET) $(XMPP_TARGET) visualptt-collector \
	      tests/test-recording tests/test-annotations tests/test-receiver-companions tests/test-collector tests/test-collector-sanitize

# -----------------------------------------------------------
# Install and uninstall
# -----------------------------------------------------------

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

install: $(REC_TARGET) $(SPOOL_TARGET) $(LIST_TARGET) $(COMBINED_TARGET) \
	$(WATCHER_TARGET) $(XMPP_TARGET) visualptt-collector
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(REC_TARGET)  $(DESTDIR)$(BINDIR)/
	install -m 0755 $(SPOOL_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0755 $(LIST_TARGET)  $(DESTDIR)$(BINDIR)/
	install -m 0755 $(COMBINED_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0755 $(WATCHER_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0755 $(XMPP_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 0755 visualptt-collector $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(REC_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(SPOOL_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(LIST_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(COMBINED_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(WATCHER_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(XMPP_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/visualptt-collector

test-xmpp: $(XMPP_TARGET)
	sh tests/test-xmpp-send.sh ./$(XMPP_TARGET)

.PHONY: all clean install uninstall test-xmpp

# Native collector: no interpreter, GTK, GStreamer or network dependencies.
COLLECTOR_CFLAGS = $(shell pkg-config --cflags sqlite3)
COLLECTOR_LIBS = $(shell pkg-config --libs sqlite3)

visualptt-collector: visualptt-collector.c recording.o sha256.o sha256.h
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic $(COLLECTOR_CFLAGS) -o $@ $(filter %.c %.o,$^) $(COLLECTOR_LIBS)

recording.o: recording.c recording.h
	$(CC) $(CFLAGS) -std=c11 -Wextra -c $< -o $@

$(REC_OBJ) $(COMBINED_OBJ) $(WATCHER_OBJ): recording.h

install-collector: visualptt-collector
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 visualptt-collector $(DESTDIR)$(BINDIR)/

.PHONY: install-collector test-delivery

tests/test-recording: tests/test-recording.c recording.o
	$(CC) $(CFLAGS) -I. $(GST_CFLAGS) -Wl,--wrap=time -Wl,--wrap=getrandom -o $@ $^ $(GST_LIBS)

tests/test-annotations: tests/test-annotations.c annotation_watcher.c recording.o
	$(CC) $(CFLAGS) -I. -o $@ tests/test-annotations.c recording.o

test-delivery: visualptt-collector tests/test-recording tests/test-annotations tests/test-receiver-companions tests/test-collector
	./tests/test-recording
	./tests/test-annotations
	./tests/test-receiver-companions
	./tests/test-collector

tests/test-receiver-companions: tests/test-receiver-companions.c visualptt-rx-list.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) -I. $(GST_CFLAGS) $(GTK_CFLAGS) -o $@ tests/test-receiver-companions.c $(COMMON_OBJ) $(GST_LIBS) $(GTK_LIBS)


tests/test-collector: tests/test-collector.c visualptt-collector.c recording.o sha256.o
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic $(COLLECTOR_CFLAGS) -Wl,--wrap=write -o $@ tests/test-collector.c recording.o sha256.o $(COLLECTOR_LIBS)

tests/test-collector-sanitize: tests/test-collector.c visualptt-collector.c recording.c recording.h sha256.c sha256.h
	$(CC) -Wall -Wextra -Wpedantic -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(COLLECTOR_CFLAGS) -Wl,--wrap=write -o $@ tests/test-collector.c recording.c sha256.c $(COLLECTOR_LIBS)

test-collector: visualptt-collector tests/test-collector
	./tests/test-collector

test-collector-sanitize: visualptt-collector tests/test-collector-sanitize
	./tests/test-collector-sanitize

.PHONY: test-collector test-collector-sanitize

sha256.o: sha256.c sha256.h
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -c $< -o $@
