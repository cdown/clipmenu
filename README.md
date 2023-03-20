[![Tests](https://img.shields.io/travis/cdown/clipmenu/develop.svg)](https://travis-ci.org/cdown/clipmenu)

clipmenu is a simple clipboard manager using [dmenu][]* and [xsel][].

*Or another [supported launcher](#supported-launchers)

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

All args passed to clipmenu are transparently dispatched to dmenu (or another
[supported launcher](#supported-launchers)). That is, if you usually call dmenu
with args to set colours and other properties, you can invoke clipmenu in
exactly the same way to get the same effect, like so:

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
- `rofi-script`, for [rofi's script mode][]

Additionally, clipmenu supports a [custom launcher mode][] for implementing
custom launcher behavior.

## The clipmenu launcher protocol

The clipmenu launcher protocol consists of three interprocess communication
methods.  clipmenu calls the launcher command as follows:

1. With its standard input set to a newline-separated stream of available
   clipboard selections,
2. With a potentially-empty list of command line arguments, and
3. With certain environment variables set (in [custom launcher mode][]).

The command line arguments convention is:

    <launcher-command> [<launcher-specific-arguments...>] [<pass-through-arguments...>]

Where:

1. `<launcher-specific-arguments...>` is a potentially-empty list of arguments
   specific to [the selected launcher](#specifying-a-launcher), and
2. `<pass-through-arguments...>` is the potentially-empty list of arguments
   provided to [clipmenu itself](#clipmenu).

For instance, when invoked like so:

    CM_LAUNCHER=rofi CM_HISTLENGTH=12 clipmenu -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

clipmenu will invoke rofi like so:

    rofi -dmenu -p clipmenu -l 12 -i -fn Terminus:size=8 -nb '#002b36' -nf '#839496' -sb '#073642' -sf '#93a1a1'

## Specifying a launcher

To tell clipmenu which launcher to use, define `CM_LAUNCHER` as follows:

    # clipmenu uses the command named `mylauncher`
    CM_LAUNCHER=mylauncher

Or:

    # clipmenu uses the specified launcher path
    CM_LAUNCHER=/path/to/mylauncher

## Specifying the launcher type

clipmenu supports [several launchers](#supported-launchers), primarily by
passing appropriate command-line arguments to the command specified in the
`CM_LAUNCHER` environment variable (or `dmenu` if `CM_LAUNCHER` is unset).

By default, clipmenu detects the launcher type by inspecting the basename of
the launcher command.  That is, when the basename of `CM_LAUNCHER` is `dmenu`,
clipmenu will call the launcher with `dmenu`-appropriate arguments, when the
basename is `rofi`, clipmenu will call the launcher with `rofi`-appropriate
arguments, and so on.

To override this detection logic, you can define the `CM_LAUNCHER_TYPE`
environment variable.  Supported values are:

1. `dmenu` - pass dmenu-appropriate arguments
2. `fzf` - pass fzf-appropriate arguments
3. `rofi` - pass rofi-appropriate arguments
4. `rofi-script` - follow [rofi's script mode][] protocol
5. `custom` - see the section on [custom launcher mode][]

Note that if you:

1. Set `CM_LAUNCHER_TYPE` to any value other than the five listed above,

**or**

2. Leave `CM_LAUNCHER_TYPE` unset and set `CM_LAUNCHER` to a value whose
   basename is something other than `dmenu`, `fzf`, or `rofi`,

then clipmenu assumes `CM_LAUNCHER` is dmenu-compatible; that is, clipmenu
will invoke the launcher with dmenu-appropriate arguments.

## Using a custom launcher

In custom laucher mode, clipmenu follows the typical [launcher
protocol](#the-clipmenu-launcher-protocol) and also sets the following
environment variables:

1. `CM_DMENU_ARGS` - contains shell-quoted arguments appropriate to pass to
   `dmenu`
2. `CM_FZF_ARGS` - contains shell-quoted arguments appropriate to pass to
   `fzf`
3. `CM_ROFI_ARGS` - contains shell-quoted arguments appropriate to pass to
   `rofi`

You can use this information for purposes like selecting a "real" launcher
implementation based upon whether you've started clipmenu from a terminal or
not:

    if detect-terminal-somehow; then
        exec fzf $CM_FZF_ARGS "$@"
    else
        exec dmenu $CM_DMENU_ARGS "$@"
    fi

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
[rofi's script mode]: https://github.com/davatorium/rofi-scripts/tree/master/mode-scripts
[custom launcher mode]: #using-a-custom-launcher
