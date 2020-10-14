# `dmenu` is not a hard dependency, but you need it unless
# you plan to set CM_LAUNCHER to another value like `rofi`
REQUIRED_BINS := xsel clipnotify
PREFIX ?= /usr
$(foreach bin,$(REQUIRED_BINS),\
    $(if $(shell command -v $(bin) 2> /dev/null),$(info Found `$(bin)`),$(error Missing Dep. Please install `$(bin)`)))

.PHONY: install

install:
	install --target "${PREFIX}/bin" -D -m755 clipmenu clipmenud clipdel clipctl
	install -D -m644 init/clipmenud.service "${PREFIX}/lib/systemd/user/clipmenud.service"
