# Makefile for plexmon

CC = cc
CFLAGS = -I/usr/local/include -Wall -Wextra -g -o2
LDFLAGS = -L/usr/local/lib -lcurl -ljson-c

# Source and header files
SRC = src/main.c src/config.c src/monitor.c src/plexapi.c src/events.c src/dircache.c src/utilities.c src/logger.c src/queue.c
OBJ = $(SRC:.c=.o)
TARGET = plexmon

# Installation directories
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
ETCDIR = $(PREFIX)/etc
RCDIR = $(ETCDIR)/rc.d

# Default target
all: $(TARGET)

# Compile source files with header dependencies
%.o: %.c $(SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Link object files
$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $(TARGET)
	strip $(TARGET)

# Install the program
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(ETCDIR)
	install -m 644 etc/plexmon.conf.sample $(DESTDIR)$(ETCDIR)/
	install -d $(DESTDIR)$(RCDIR)
	install -m 755 etc/plexmon.rc $(DESTDIR)$(RCDIR)/plexmon

# Clean up
clean:
	rm -f $(OBJ) $(TARGET)

# Help message
help:
	@echo "Available targets:"
	@echo "  all       - Build plexmon"
	@echo "  install   - Install plexmon to $(BINDIR)"
	@echo "  clean     - Remove build files"
	@echo "  help      - Show this help message"

.PHONY: all install clean help