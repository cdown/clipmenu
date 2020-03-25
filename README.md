[![Tests](https://img.shields.io/travis/cdown/clipmenu/develop.svg)](https://travis-ci.org/cdown/clipmenu)

clipmenu is a simple clipboard manager using [dmenu][] (or [rofi][] with
`CM_LAUNCHER=rofi`) and [xsel][].

# Demo

![Demo](https://cloud.githubusercontent.com/assets/660663/24079784/6f76da94-0c88-11e7-8251-40b1f02ebf3c.gif)

# Usage

Start `clipmenud`, then run `clipmenu` to select something to put on the
clipboard.

A systemd user service for starting clipmenud is installed as part of the project.

If you want to start `clipmenud` immediately via systemd and have it be started when you login, run:

    systemctl --user enable --now clipmenud

You may wish to bind a shortcut in your window manager to launch `clipmenu`.

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect, like so:

    clipmenu -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

If you prefer to collect clips on demand rather than running clipmenud as a
daemon, you can bind a key to the following command for one-off collection:

    CM_ONESHOT=1 clipmenud

For a full list of environment variables that clipmenud can take, please see
`clipmenud --help`.

# Installation

Several distributions, including Arch and Nix, provide clipmenu as an official
package called `clipmenu`.

## Installation - Manual

If your distribution doesn't provide a package, you can run the following
the commands to install this.  (Or better yet, create package for your distribution!).
You'll first need to install `xsel` and `clipnotify`. If you'll also need `dmenu` unless
you plan to set `CM_LAUNCHER` to a different value, like `rofi`.

     git clone https://github.com/cdown/clipmenu.git
     cd clipmenu
     sudo make install

# How does it work?

clipmenud is less than 200 lines, and clipmenu is less than 100, so hopefully
it should be fairly self-explanatory. However, at the most basic level:

## clipmenud

1. `clipmenud` uses [clipnotify](https://github.com/cdown/clipnotify) to wait
   for new clipboard events.
2. If `clipmenud` detects changes to the clipboard contents, it writes them out
   to the cache directory and an index using a hash as the filename.

### Features of `clipmenud`

The behavior of `clipmenud` can be customized through environment variables. Features include:

 * Customizing max number of clips (Default: 1000)
 * Choosing which selections to manage
 * Disabling clip collection temporarily with `clipctl disable`, reenabling with `clipctl enable`
 * Ignoring certain windows, like password managers
 * Enabling debugging
 * Customizing the cache dir location
 * Disable looping
 * Option to "own" the clipboard

Check the online help to view the details:

    clipmenud --help

If you managing `clipmenud` with `systemd`, you can override the defaults by using this command to generate an override file:

    systemctl --user edit clipmenud

Then add a new section sets your environment variables. For example:

```
[Service]
Environment="CM_MAX_CLIPS=30"
Environment="CM_SELECTIONS=clipboard"
```

## clipmenu

1. `clipmenu` reads the index to find all available clips.
2. `dmenu` is executed to allow the user to select a clip.
3. After selection, the clip is put onto the PRIMARY and CLIPBOARD X
   selections.

[dmenu]: http://tools.suckless.org/dmenu/
[rofi]: https://github.com/DaveDavenport/Rofi
[xsel]: http://www.vergenet.net/~conrad/software/xsel/
