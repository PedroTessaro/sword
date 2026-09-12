CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX   ?= $(HOME)/.local
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
RTOBJ    := rt/sword_rt.o rt/sword_net.o rt/sword_os.o rt/sword_poll.o \
            rt/sword_fs.o rt/sword_ctx.o

all: $(BIN) $(LSP) $(RT)

$(BIN): $(COREOBJ) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(LSP): $(COREOBJ) $(LSPOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Linked into every compiled program that spawns anything.
$(RT): $(RTOBJ)
	ar rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.S
	$(CXX) -c $< -o $@

-include $(OBJ:.o=.d) $(RTOBJ:.o=.d)

test: $(BIN) $(LSP) $(RT)
	@./tests/run.sh
	@./tests/lsp.sh
	@./tests/std.sh

# Installed layout: binaries in bin/, the runtime archive in lib/sword and the
# standard library in share/sword, which is where the compiler looks for them.
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/sword
	install -d $(DESTDIR)$(PREFIX)/share/sword/std
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -m 755 $(LSP) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	install -m 644 $(RT) $(DESTDIR)$(PREFIX)/lib/sword/$(RT)
	cp -R std/. $(DESTDIR)$(PREFIX)/share/sword/std/
	cp -R editors $(DESTDIR)$(PREFIX)/share/sword/
	@echo
	@echo "Installed to $(DESTDIR)$(PREFIX)."
	@echo "If 'shield' is not found, add $(PREFIX)/bin to your PATH."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	rm -rf $(DESTDIR)$(PREFIX)/lib/sword $(DESTDIR)$(PREFIX)/share/sword

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RTOBJ) $(RTOBJ:.o=.d) $(BIN) $(LSP) $(RT)

.PHONY: all test install uninstall clean
