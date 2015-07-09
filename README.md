clipmenu is a simple clipboard manager using [dmenu][] and [xsel][].

To use it, start the `clipmenud` daemon, and then call `clipmenu` to launch
`dmenu`. Upon choosing an entry, it is copied to the clipboard.

All args passed to clipmenu are transparently dispatched to dmenu. That is, if
you usually call dmenu with args to set colours and other properties, you can
invoke clipmenu in exactly the same way to get the same effect.

[dmenu]: http://tools.suckless.org/dmenu/
[xsel]: http://www.vergenet.net/~conrad/software/xsel/

## [dwm](http://dwm.suckless.org/) configuration

	static const char *urlcmd[]  = { "clipmenu-url", NULL };
	static const char *clipcmd[]  = { "clipmenu", "-fn", dmenufont, NULL };

MODKEY is alt by default in dwm. So Alt+Insert is the clipboard history menu. Alt+o opens the last URL in your clipboard.

	{ MODKEY,                       XK_Insert, spawn,          {.v = clipcmd } },
	{ MODKEY,                       XK_o,      spawn,          {.v = urlcmd } },
