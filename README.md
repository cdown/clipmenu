clipmenu is a simple clipboard manager using [dmenu][], [rofi][] or similar.

# Demo

![Demo](https://cloud.githubusercontent.com/assets/660663/24079784/6f76da94-0c88-11e7-8251-40b1f02ebf3c.gif)

# Usage

## clipmenud

Start `clipmenud`, then run `clipmenu` to select something to put on the
clipboard. For systemd users, a user service called `clipmenud` is packaged as
part of the project.

If you start X via `startx`/`xinit` and use the systemd user service, make sure
`$DISPLAY` is set so clipmenud knows which X server to use. Most distributions
include scripts in `/etc/X11/xinit/xinitrc.d/` for this, so if they are present
you can usually just source them in your `~/.xinitrc`. Alternatively, before
launching the clipmenud service, run:

    systemctl --user import-environment DISPLAY

## clipmenu

You may wish to bind a shortcut in your window manager to launch `clipmenu`.

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect, like so:

    clipmenu -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

For a full list of environment variables that clipmenud can take, please see
`man clipmenud`.

There is also `clipdel` to delete clips, and `clipctl` to enable or disable
clipboard monitoring.

# Features

The behavior of `clipmenud` can be customized through a config file. As some
examples of things you can change:

* Customising the maximum number of clips stored (default 1000)
* Disabling clip collection temporarily with `clipctl disable`, reenabling with
  `clipctl enable`
* Not storing clipboard changes from certain applications, like password
  managers
* Taking direct ownership of the clipboard
* ...and much more.

See `man clipmenu.conf` to view all possible configuration variables and what
they do.

# Supported launchers

Any dmenu-compliant application will work, but here are `CM_LAUNCHER`
configurations that are known to work:

- `dmenu` (the default)
- `fzf`
- `rofi`

# Installation

Several distributions, including Arch and Nix, provide clipmenu as an official
package called `clipmenu`.

## Manual installation

If your distribution doesn't provide a package, you can manually install using
`make install` (or better yet, create a package for your distribution!).

# How does it work?

## clipmenud

1. clipmenud passively monitors X11 clipboard selections (PRIMARY, CLIPBOARD,
   and SECONDARY) for changes using XFixes (no polling).
2. If `clipmenud` detects changes to the clipboard contents, it writes them out
   to storage and indexes using a hash as the filename.

## clipmenu

1. `clipmenu` reads the index to find all available clips.
2. `dmenu` (or another configured launcher) is executed to allow the user to
   select a clip.
3. After selection, the clip is put onto the PRIMARY and CLIPBOARD X
   selections.

[dmenu]: http://tools.suckless.org/dmenu/
[rofi]: https://github.com/DaveDavenport/Rofi
