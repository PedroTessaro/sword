CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

CORE     := $(filter-out src/main.cpp, $(wildcard src/*.cpp))
COREOBJ  := $(CORE:.cpp=.o)
OBJ      := $(COREOBJ) src/main.o
BIN      := shield
RT       := libsword_rt.a
RTOBJ    := rt/sword_rt.o

all: $(BIN) $(RT)

$(BIN): $(COREOBJ) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# Linked into every compiled program that spawns anything. Being an archive,
# a program that never does carries none of it.
$(RT): $(RTOBJ)
	ar rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d) $(RTOBJ:.o=.d)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RTOBJ) $(RTOBJ:.o=.d) $(BIN) $(RT)

.PHONY: all clean
