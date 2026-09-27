# N_THREADS = 4
N_THREADS = 24 # If default value in source files is preferred, then comment out this line like above

CC = g++-10
CXXFLAGS = -g3 -O0 -std=c++20 -Wall -Wextra -Wpedantic
LDLIBS = -pthread
SRC = main.cpp
INCLUDE =
TARGET = main

ifdef N_THREADS
	CXXFLAGS += -DN_THREADS=$(N_THREADS)
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(INCLUDE) Makefile
	$(CC) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)
