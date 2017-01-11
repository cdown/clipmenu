clipmenu is a simple clipboard manager using [dmenu][] and [xsel][].

# Usage

Start `clipmenud`, then run `clipmenu` to select something to put on the
clipboard.

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect, like so:

    clipmenu -i -fn Terminus:size=8 -nb #002b36 -nf #839496 -sb #073642 -sf #93a1a1

# How does it work?

The code is fairly simple and easy to follow, you may find it easier to read
there, but it basically works like this:

## clipmenud

1. `clipmenud` polls the clipboard every 0.5 seconds (or another interval as
   configured with the `CLIPMENUD_SLEEP` environment variable). Unfortunately
   there's no interface to subscribe for changes in X11, so we must poll.
2. If `clipmenud` detects changes to the clipboard contents, it writes them out
   to the cache directory.

## clipmenu

1. `clipmenu` reads the cache directory to find all available clips.
2. `dmenu` is executed to allow the user to select a clip.
3. After selection, the clip is put onto the PRIMARY and CLIPBOARD X
   selections.

[dmenu]: http://tools.suckless.org/dmenu/
[xsel]: http://www.vergenet.net/~conrad/software/xsel/
