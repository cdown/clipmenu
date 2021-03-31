[![Tests](https://img.shields.io/travis/cdown/clipmenu/develop.svg)](https://travis-ci.org/cdown/clipmenu)

clipmenu is a simple clipboard manager using [dmenu][] (or [rofi][] with
`CM_LAUNCHER=rofi`) and [xsel][].

# Demo

![Demo](https://cloud.githubusercontent.com/assets/660663/24079784/6f76da94-0c88-11e7-8251-40b1f02ebf3c.gif)

# Usage

## clipmenud

Start `clipmenud`, then run `clipmenu` to select something to put on the
clipboard. For systemd users, a user service called `clipmenud` is packaged as
part of the project.

For those using a systemd unit and not using a desktop environment which does
it automatically, you must import `$DISPLAY` so that `clipmenud` knows which X
server to use. For example, in your `~/.xinitrc` do this prior to launching
clipmenud:

    systemctl --user import-environment DISPLAY

## clipmenu

You may wish to bind a shortcut in your window manager to launch `clipmenu`.

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect, like so:

    clipmenu -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

For a full list of environment variables that clipmenud can take, please see
`clipmenud --help`.

# Features

The behavior of `clipmenud` can be customized through environment variables.
Despite being only <300 lines, clipmenu has many useful features, including:

* Customising the maximum number of clips stored (default 1000)
* Disabling clip collection temporarily with `clipctl disable`, reenabling with
  `clipctl enable`
* Not storing clipboard changes from certain applications, like password
  managers
* Taking direct ownership of the clipboard
* ...and much more.

Check `clipmenud --help` to view all possible environment variables and what
they do. If you manage `clipmenud` with `systemd`, you can override the
defaults by using `systemctl --user edit clipmenud` to generate an override
file.

# Supported launchers

Any dmenu-compliant application will work, but here are `CM_LAUNCHER`
configurations that are known to work:

- `dmenu` (the default)
- `fzf`
- `rofi`
- `rofi-script`, for [rofi's script
  mode](https://github.com/davatorium/rofi-scripts/tree/master/mode-scripts)

# Installation

Several distributions, including Arch and Nix, provide clipmenu as an official
package called `clipmenu`.

## Manual installation

If your distribution doesn't provide a package, you can manually install using
`make install` (or better yet, create a package for your distribution!). You
will need `xsel` and `clipnotify` installed, and also `dmenu` unless you plan
to use a different launcher.

# How does it work?

clipmenud is less than 300 lines, and clipmenu is less than 100, so hopefully
it should be fairly self-explanatory. However, at the most basic level:

## clipmenud

1. `clipmenud` uses [clipnotify](https://github.com/cdown/clipnotify) to wait
   for new clipboard events.
2. If `clipmenud` detects changes to the clipboard contents, it writes them out
   to the cache directory and an index using a hash as the filename.

## clipmenu

1. `clipmenu` reads the index to find all available clips.
2. `dmenu` is executed to allow the user to select a clip.
3. After selection, the clip is put onto the PRIMARY and CLIPBOARD X
   selections.

[dmenu]: http://tools.suckless.org/dmenu/
[rofi]: https://github.com/DaveDavenport/Rofi
[xsel]: http://www.vergenet.net/~conrad/software/xsel/
