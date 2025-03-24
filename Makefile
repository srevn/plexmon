# Makefile for plexmon

CC = cc
CFLAGS = -I/usr/local/include -Wall -Wextra -g
LDFLAGS = -L/usr/local/lib -lcurl -ljson-c

# Source files
SRC = src/main.c src/config.c src/fsmonitor.c src/plexapi.c src/events.c src/dircache.c src/utilities.c
OBJ = $(SRC:.c=.o)
TARGET = plexmon

# Installation directories
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
ETCDIR = $(PREFIX)/etc
RCDIR = $(ETCDIR)/rc.d

# Default target
all: $(TARGET)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link object files
$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $(TARGET)

# Install the program
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(ETCDIR)
	install -m 644 plexmon.conf.sample $(DESTDIR)$(ETCDIR)/
	install -d $(DESTDIR)$(RCDIR)
	install -m 755 rc.d/plexmon $(DESTDIR)$(RCDIR)/

# Clean up
clean:
	rm -f $(OBJ) $(TARGET)

# Run with valgrind for memory leak detection
memcheck: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET) -v

# Help message
help:
	@echo "Available targets:"
	@echo "  all       - Build plexmon"
	@echo "  install   - Install plexmon to $(BINDIR)"
	@echo "  clean     - Remove build files"
	@echo "  memcheck  - Run with valgrind memory checker"
	@echo "  help      - Show this help message"

.PHONY: all install clean memcheck help