CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wshadow -Wpointer-arith \
	  -Wcast-align -Wmissing-prototypes -Wstrict-overflow -Wformat=2 \
	  -Wwrite-strings -Warray-bounds -Wstrict-prototypes \
	  -Wno-maybe-uninitialized \
	  -Werror $(CFLAGS)
CPPFLAGS += -I/usr/X11R6/include -L/usr/X11R6/lib
LDLIBS += -lX11 -lXfixes
PREFIX ?= /usr/local
bindir := $(PREFIX)/bin
datarootdir := $(PREFIX)/share
mandir := $(datarootdir)/man
systemd_user_dir = $(DESTDIR)$(PREFIX)/lib/systemd/user
debug_cflags := -D_FORTIFY_SOURCE=2 -fsanitize=leak -fsanitize=address \
	        -fsanitize=undefined -Og -ggdb -fno-omit-frame-pointer \
	        -fstack-protector-strong
c_files := $(wildcard src/*.c)
h_files := $(wildcard src/*.h)
libs := $(filter $(c_files:.c=.o), $(h_files:.h=.o))

man1_files = clipctl.1 clipdel.1 clipmenu.1 clipmenud.1 clipserve.1
man5_files = clipmenu.conf.5

bins := clipctl clipmenud clipdel clipserve clipmenu

all: $(addprefix src/,$(bins))

src/%: src/%.c $(libs)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

src/%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

debug: all
debug: CFLAGS+=$(debug_cflags)

install: all
	@for f in $(man1_files); do \
		install -Dp -m 644 man/$$f $(DESTDIR)$(mandir)/man1/$$f; \
	done
	@for f in $(man5_files); do \
		install -Dp -m 644 man/$$f $(DESTDIR)$(mandir)/man5/$$f; \
	done

	mkdir -p $(DESTDIR)$(bindir)/
	install -pt $(DESTDIR)$(bindir)/ $(addprefix src/,$(bins))
	mkdir -p $(systemd_user_dir)
	sed 's|@bindir@|$(bindir)|g' init/clipmenud.service.in > $(systemd_user_dir)/clipmenud.service

uninstall:
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/bin/,$(bins))
	rm -f "$(DESTDIR)${PREFIX}/lib/systemd/user/clipmenud.service"

clean:
	rm -f src/*.o src/*~ $(addprefix src/,$(bins))

clang_supports_unsafe_buffer_usage := $(shell clang -x c -c /dev/null -o /dev/null -Werror -Wunsafe-buffer-usage > /dev/null 2>&1; echo $$?)
ifeq ($(clang_supports_unsafe_buffer_usage),0)
    extra_clang_flags := -Wno-unsafe-buffer-usage
else
    extra_clang_flags :=
endif

c_analyse_targets := $(c_files:%=%-analyse)
h_analyse_targets := $(h_files:%=%-analyse)

analyse: CFLAGS+=$(debug_cflags)
analyse: $(c_analyse_targets) $(h_analyse_targets)

$(c_analyse_targets): %-analyse:
	# -W options here are not clang compatible, so out of generic CFLAGS
	gcc $< -o /dev/null -c \
		-std=gnu99 -Ofast -fwhole-program -Wall -Wextra \
		-Wlogical-op -Wduplicated-cond \
		-fanalyzer $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS)
	clang $< -o /dev/null -c -std=gnu99 -Ofast -Weverything \
		-Wno-documentation-unknown-command \
		-Wno-language-extension-token \
		-Wno-disabled-macro-expansion \
		-Wno-padded \
		-Wno-covered-switch-default \
		-Wno-gnu-zero-variadic-macro-arguments \
		-Wno-declaration-after-statement \
		-Wno-cast-qual \
		-Wno-unused-command-line-argument \
		$(extra_clang_flags) \
		$(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS)
	$(MAKE) $*-shared-analyse

$(h_analyse_targets): %-analyse:
	$(MAKE) $*-shared-analyse

%-shared-analyse: %
	# cppcheck is a bit dim about unused functions/variables, leave that to
	# clang/GCC
	cppcheck $< --std=c99 --quiet --inline-suppr --force \
		--enable=all --suppress=missingIncludeSystem \
		--suppress=unusedFunction --suppress=unmatchedSuppression \
		--suppress=unreadVariable \
		--suppress=checkersReport \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=unusedStructMember \
		--max-ctu-depth=32 --error-exitcode=1
	# clang-analyzer-unix.Malloc does not understand _drop_()
	clang-tidy $< --quiet -checks=-clang-analyzer-unix.Malloc -- -std=gnu99
	clang-format --dry-run --Werror $<

tests: tests/test_store
	tests/test_store

integration_tests:
	tests/x_integration_tests

tests/test_store: tests/test_store.c src/store.o src/util.o
	$(CC) $(CFLAGS) $(CPPFLAGS) -I./src -o $@ $^ $(LDLIBS)

.PHONY: all debug install uninstall clean analyse tests integration_tests
