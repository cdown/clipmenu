[![Tests](https://img.shields.io/travis/cdown/clipmenu/develop.svg)](https://travis-ci.org/cdown/clipmenu)

clipmenu is a simple clipboard manager using [dmenu][] (or [rofi][] with
`CM_LAUNCHER=rofi`) and [xsel][].

# Demo

![Demo](https://cloud.githubusercontent.com/assets/660663/24079784/6f76da94-0c88-11e7-8251-40b1f02ebf3c.gif)

# Usage

Start `clipmenud`, then run `clipmenu` to select something to put on the
clipboard.

A systemd user service for starting clipmenud is included at
[init/clipmenud.service](https://github.com/cdown/clipmenu/blob/develop/init/clipmenud.service).

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect, like so:

    clipmenu -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

You can remove clips with the `clipdel` utility, see `clipdel --help`.

# Installation

Several distributions, including Arch and Nix, provide clipmenu as an official
package called `clipmenu`.

If your distribution doesn't provide a package, you can run the scripts
standalone (or better yet, package them!).

# How does it work?

The code is fairly simple and easy to follow, you may find it easier to read
there, but it basically works like this:

## clipmenud

1. `clipmenud` uses [clipnotify](https://github.com/cdown/clipnotify) to wait
   for new clipboard events. If clipnotify is not present on the system, we
   poll every 0.5 seconds (or another interval as configured with the
   `CM_SLEEP` environment variable).

   You can also bind your copy key binding to also issue `CM_ONESHOT=1
   clipmenud`. However, there's no generic way to do this, since any keys or
   mouse buttons could be bound to do this action in a number of ways.
2. If `clipmenud` detects changes to the clipboard contents, it writes them out
   to the cache directory.

### Features of `clipmenud`

The behavior of `clipmenud` can be customized through environment variables. Features include:

 * Customizing max number of clips (Default: 1000)
 * Choosing which selections to manage
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

1. `clipmenu` reads the cache directory to find all available clips.
2. `dmenu` is executed to allow the user to select a clip.
3. After selection, the clip is put onto the PRIMARY and CLIPBOARD X
   selections.

[dmenu]: http://tools.suckless.org/dmenu/
[rofi]: https://github.com/DaveDavenport/Rofi
[xsel]: http://www.vergenet.net/~conrad/software/xsel/
