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

## Sponsors

We would like to sincerely thank our sponsor [grommunio}(https://grommunio.com/) for supporting our software project. Your contribution helps us continue developing, improving, and providing valuable solutions to our users.

We truly appreciate your support and partnership!
