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

man1_files = clipctl.1 clipdel.1 clipdelmenu.1 clipmenu.1 clipmenud.1 clipserve.1
man5_files = clipmenu.conf.5

bins := clipctl clipmenud clipdel clipdelmenu clipserve clipmenu

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
	rm -f $(addprefix $(DESTDIR)$(mandir)/man1/,$(man1_files))
	rm -f $(addprefix $(DESTDIR)$(mandir)/man5/,$(man5_files))

clean:
	rm -f src/*.o src/*~ $(addprefix src/,$(bins)) tests/test_store

clang_supports_unsafe_buffer_usage := $(shell clang -x c -c /dev/null -o /dev/null -Werror -Wunsafe-buffer-usage > /dev/null 2>&1; echo $$?)
ifeq ($(clang_supports_unsafe_buffer_usage),0)
    extra_clang_flags := -Wno-unsafe-buffer-usage -Wno-missing-include-dirs \
			 -Wno-unknown-warning-option \
			 -Wno-unused-command-line-argument \
			 -Wno-error
else
    extra_clang_flags := -Wno-missing-include-dirs \
			 -Wno-unknown-warning-option \
			 -Wno-unused-command-line-argument \
			 -Wno-error
endif

c_analyse_targets := $(c_files:%=%-analyse)
h_analyse_targets := $(h_files:%=%-analyse)

analyse: CFLAGS+=$(debug_cflags)
analyse: cppcheck $(c_analyse_targets) $(h_analyse_targets)

$(c_analyse_targets): %-analyse: %
	# -W options here are not clang compatible, so out of generic CFLAGS
	gcc $< -o /dev/null -c \
		-std=gnu99 -Ofast -fwhole-program -Wall -Wextra \
		-Wlogical-op -Wduplicated-cond \
		-fanalyzer $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS)
	clang $< -o /dev/null -c -std=gnu99 -Ofast \
		$(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS) \
		$(extra_clang_flags)
	$(MAKE) $*-shared-analyse

$(h_analyse_targets): %-analyse:
	$(MAKE) $*-shared-analyse

%-shared-analyse: %
	# clang-analyzer-unix.Malloc does not understand _drop_()
	clang-tidy $< --quiet -checks=-clang-analyzer-unix.Malloc -- -std=gnu99
	clang-format --dry-run --Werror $<

# --suppress=missingIncludeSystem:
#
# Without this there's a bunch of noise from cppcheck from the system headers
# themselves.
#
# --suppress=unusedFunction:
#
# cppcheck does not understand _drop_ and marks those as unused. This is
# already well checked by Clang/GCC, just leave it to them.
#
# --suppress=unmatchedSuppression:
#
# We run both locally and on CI, so there may be some suppressions that
# depending on version do not match in one version but do on another.
#
# --suppress=unusedStructMember
#
# Structs are used across translation units and cppcheck gets this wrong.
cppcheck: $(c_files) $(h_files)
	cppcheck $(c_files) $(h_files) --std=c99 --quiet --inline-suppr --force \
		--enable=all \
		--suppress=missingIncludeSystem \
		--suppress=unusedFunction \
		--suppress=unmatchedSuppression \
		--suppress=checkersReport \
		--suppress=unusedStructMember \
		--check-level=exhaustive \
		--max-ctu-depth=10 --error-exitcode=1

tests: tests/test_store
	tests/test_store

integration_tests:
	tests/x_integration_tests

tests/test_store: tests/test_store.c src/store.o src/util.o
	$(CC) $(CFLAGS) $(CPPFLAGS) -I./src -o $@ $^ $(LDLIBS)

.PHONY: all debug install uninstall clean analyse tests integration_tests \
	cppcheck
