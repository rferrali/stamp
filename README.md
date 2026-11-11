> [!WARNING]
> **Personal fork. Not maintained for anyone but me.**
> This is my own copy of [Stamp](https://gitlab.gnome.org/jbrummer/stamp) with a few small, AI-assisted changes for my own setup. Use upstream instead.
> Issues are off and I won't review pull requests. If anything here is useful, especially to upstream, feel free to take it.
> My changes live on the `mine` branch, on top of upstream `main`: [see the diff](https://github.com/rferrali/stamp/compare/2a33088049327ea1238f68b6e479f9893ae85cf7...mine).

# Stamp

A modern mail suite written in C using GTK4 and Adwaita, built on the Evolution Data Server stack.

## Features

- **Mail** - Send, receive, and manage mails with full MIME support via Camel
- **Contacts** - Integrated address book management using EDS (libebook)
- **Calendar** - Calendar support via GNOME Calendar integration (pending)
- **PGP and S/MIME** - Sign and encrypt emails with OpenPGP and S/MIME
- **BIMI** - Brand Indicators for Message Identification support
- **One-Click Unsubscribe** - Offer simple one click unsubscribe from mailing lists 

## Building

Stamp uses the [Meson](https://mesonbuild.com) build system.

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

## License

Stamp is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
