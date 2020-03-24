# `dmenu` is not a hard dependency, but you need it unless
# you plan to set CM_LAUNCHER to another value like `rofi`
REQUIRED_BINS := xsel clipnotify
$(foreach bin,$(REQUIRED_BINS),\
    $(if $(shell command -v $(bin) 2> /dev/null),$(info Found `$(bin)`),$(error Missing Dep. Please install `$(bin)`)))

.PHONY: install

install:
	install -D -m755 clipmenu /usr/bin/clipmenu
	install -D -m755 clipmenud /usr/bin/clipmenud
	install -D -m755 clipdel /usr/bin/clipdel
	install -D -m644 init/clipmenud.service /usr/lib/systemd/user/clipmenud.service
