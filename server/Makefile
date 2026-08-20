CROSS_COMPILE ?=

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -g -pthread

TARGET = aesdsocket
SRC = aesdsocket.c

.PHONY: all default clean

all: $(TARGET)

default: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)