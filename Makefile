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
# What a compiled program has to be linked against beyond the archive. Written
# at build time and read by the compiler, so the archive and its link line travel
# together — a program that uses TLS needs libssl, and only the build knows where
# that is on this machine.
RTFLAGS  := libsword_rt.flags
RTOBJ    := rt/sword_rt.o rt/sword_net.o rt/sword_os.o rt/sword_poll.o \
            rt/sword_fs.o rt/sword_proc.o rt/sword_chan.o rt/sword_tls.o \
            rt/sword_ctx.o

# TLS is optional: the protocol is not something to write by hand, so it is
# OpenSSL or nothing. Found here, or the runtime answers "built without TLS" and
# everything else works as before. SWORD_OPENSSL=<prefix> overrides the search;
# SWORD_NO_TLS=1 skips it.
OPENSSL_PREFIX := $(strip $(SWORD_OPENSSL))
ifeq ($(OPENSSL_PREFIX),)
OPENSSL_PREFIX := $(firstword $(wildcard \
    /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 \
    /opt/homebrew/opt/openssl /usr/local/opt/openssl /usr))
endif
ifeq ($(SWORD_NO_TLS),1)
OPENSSL_PREFIX :=
endif
ifneq ($(strip $(wildcard $(OPENSSL_PREFIX)/include/openssl/ssl.h)),)
# The system's own copy is already on every search path, and naming
# /usr/include with -I moves it ahead of the C++ library's wrappers around the
# C headers, which then cannot find what they wrap.
ifeq ($(OPENSSL_PREFIX),/usr)
TLS_CXXFLAGS := -DSWORD_HAVE_TLS=1
TLS_LDFLAGS  := -lssl -lcrypto
else
TLS_CXXFLAGS := -DSWORD_HAVE_TLS=1 -I$(OPENSSL_PREFIX)/include
TLS_LDFLAGS  := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif
TLS_STAMP    := rt/.tls-on
else
TLS_STAMP    := rt/.tls-off
endif

# The runtime is C++, so a program links against whichever standard library the
# compiler that built it uses: libc++ from Apple's, libstdc++ from GCC's. Asked
# of the compiler rather than guessed from the system, since clang on Linux can
# be either. libstdc++ is named by its directory as well: clang links against the
# newest GCC it finds, and on Ubuntu 24.04 that is a GCC 14 with no libstdc++
# beside it while g++ is 13.
CXXLIB := $(if $(shell $(CXX) -std=c++17 -dM -E -x c++ -include cstddef \
    /dev/null 2>/dev/null | grep _LIBCPP_VERSION),-lc++,-lstdc++)
ifeq ($(CXXLIB),-lstdc++)
CXXLIBDIR := $(dir $(shell $(CXX) -print-file-name=libstdc++.so))
ifneq ($(filter /%,$(CXXLIBDIR)),)
CXXLIB := -L$(CXXLIBDIR) -lstdc++
endif
endif

all: $(BIN) $(LSP) $(RT) $(RTFLAGS)

$(BIN): $(COREOBJ) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(LSP): $(COREOBJ) $(LSPOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Linked into every compiled program that spawns anything.
$(RT): $(RTOBJ)
	ar rcs $@ $^

# Checked every build, because the answer depends on what is installed on the
# machine rather than on any file make can date it against — but only written when
# it changes, so that the TLS object below is rebuilt exactly when the decision
# does.
$(RTFLAGS): FORCE
	@echo '$(TLS_LDFLAGS) $(CXXLIB)' > $@

# The decision is in the file's name, so flipping it leaves a prerequisite that
# does not exist and the object is rebuilt. Comparing timestamps would not do:
# make 3.81, which is what macOS ships, dates files to the second, and a rewrite
# in the same second as the build it should invalidate is invisible to it.
$(TLS_STAMP):
	@rm -f rt/.tls-on rt/.tls-off rt/sword_tls.o
	@touch $@

FORCE:

rt/sword_tls.o: rt/sword_tls.cpp $(TLS_STAMP)
	$(CXX) $(CXXFLAGS) $(TLS_CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.S
	$(CXX) -c $< -o $@

-include $(OBJ:.o=.d) $(RTOBJ:.o=.d)

test: $(BIN) $(LSP) $(RT) $(RTFLAGS)
	@./tests/run.sh
	@./tests/lsp.sh
	@./tests/std.sh
	@./tests/indent.sh

# Installed layout: binaries in bin/, the runtime archive in lib/sword and the
# standard library in share/sword, which is where the compiler looks for them.
# Editor support goes beside it, without what npm and the VS Code extension's test
# download into the tree: the second is a whole copy of VS Code.
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/sword
	install -d $(DESTDIR)$(PREFIX)/share/sword/std
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -m 755 $(LSP) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	install -m 644 $(RT) $(DESTDIR)$(PREFIX)/lib/sword/$(RT)
	install -m 644 $(RTFLAGS) $(DESTDIR)$(PREFIX)/lib/sword/$(RTFLAGS)
	cp -R std/. $(DESTDIR)$(PREFIX)/share/sword/std/
	tar -cf - --exclude node_modules --exclude .vscode-test --exclude '*.vsix' editors | \
	    tar -xf - -C $(DESTDIR)$(PREFIX)/share/sword
	@echo
	@echo "Installed to $(DESTDIR)$(PREFIX)."
	@echo "If 'shield' is not found, add $(PREFIX)/bin to your PATH."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	rm -rf $(DESTDIR)$(PREFIX)/lib/sword $(DESTDIR)$(PREFIX)/share/sword

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RTOBJ) $(RTOBJ:.o=.d) $(BIN) $(LSP) $(RT) \
	      $(RTFLAGS) rt/.tls-on rt/.tls-off

.PHONY: all test install uninstall clean FORCE
