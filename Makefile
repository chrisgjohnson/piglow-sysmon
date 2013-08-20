CC=g++
CFLAGS=-c -Wall 
LDFLAGS=-lrt -lwiringPi -lwiringPiDev
SOURCES=piglow-sysmon.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=piglow-sysmon

all: $(SOURCES) $(EXECUTABLE)
	
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf *.o $(EXECUTABLE)

