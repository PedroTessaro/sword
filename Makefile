CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

# The front end is shared: the language server is the same lexer, parser and
# checker behind a different mouth.
CORE     := $(filter-out src/main.cpp, $(wildcard src/*.cpp))
COREOBJ  := $(CORE:.cpp=.o)
LSPSRC   := $(wildcard lsp/*.cpp)
LSPOBJ   := $(LSPSRC:.cpp=.o)
OBJ      := $(COREOBJ) src/main.o $(LSPOBJ)
BIN      := shield
LSP      := swordls
RT       := libsword_rt.a
RTOBJ    := rt/sword_rt.o

all: $(BIN) $(LSP) $(RT)

$(BIN): $(COREOBJ) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(LSP): $(COREOBJ) $(LSPOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Linked into every compiled program that spawns anything. Being an archive,
# a program that never does carries none of it.
$(RT): $(RTOBJ)
	ar rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d) $(RTOBJ:.o=.d)

test: $(BIN) $(LSP) $(RT)
	@./tests/run.sh
	@./tests/lsp.sh

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RTOBJ) $(RTOBJ:.o=.d) $(BIN) $(LSP) $(RT)

.PHONY: all test clean
