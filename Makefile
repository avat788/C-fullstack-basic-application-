CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDLIBS  = -lsqlite3
TARGET  = app
SRC     = main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET) tasks.db
