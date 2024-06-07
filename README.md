clipmenu is a simple clipboard manager using [dmenu][] (or [rofi][] with
`CM_LAUNCHER=rofi`).

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

# Features

* Customising the maximum number of clips stored (default 1000)
* Disabling clip collection temporarily with `clipctl disable`, re-enabling with
  `clipctl enable`
* Not storing clipboard changes from certain applications, like password
  managers
* Taking direct ownership of the clipboard
* ...and much more.

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
`make install` (or better yet, create a package for your distribution!). You
will need `dmenu` installed, unless you plan to use a different launcher.

[dmenu]: http://tools.suckless.org/dmenu/
[rofi]: https://github.com/DaveDavenport/Rofi
