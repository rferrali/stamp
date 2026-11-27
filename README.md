> [!WARNING]
> **Personal fork. Not maintained for anyone but me.**
> This is my own copy of [Stamp](https://gitlab.gnome.org/jbrummer/stamp) with a few small, AI-assisted changes for my own setup. Use upstream instead.
> Issues are off and I won't review pull requests. If anything here is useful, especially to upstream, feel free to take it.
> My changes live on the `mine` branch, on top of upstream `main`: [see the whole diff](https://github.com/rferrali/stamp/compare/fork-base...mine).

## About this fork

`mine` is upstream `main` plus one merge per topic branch. Each branch holds a single change, so it can be read, dropped or sent upstream on its own. `fork-base` is the upstream commit they all sit on, and each link below shows just that branch's diff.

| Branch | Change | Upstream |
|---|---|---|
| [`feature/contact-completion-mail-history`](https://github.com/rferrali/stamp/compare/fork-base...feature/contact-completion-mail-history) | When recipient completion searches the mail history, it suggests the people you sent mail to (To/Cc), not only people who wrote to you, and it reads every address in a header, not just the first. | [#275](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/275) (follow-up to [#245](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/245), whose other half upstream fixed) |
| [`fix/composer-file-drop`](https://github.com/rferrali/stamp/compare/fork-base...fix/composer-file-drop) | Dropping files from another application, such as Files, on the composer attaches them again. It stopped working on 31 August, when the drop check started refusing anything that came from outside Stamp. | [#276](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/276) |
| [`fix/forward-attachments`](https://github.com/rferrali/stamp/compare/fork-base...fix/forward-attachments) | Forwarding a mail also forwards its attachments, which show up in the composer and can be removed before sending. | [#277](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/277) |
| [`fix/ews-sent-copy`](https://github.com/rferrali/stamp/compare/fork-base...fix/ews-sent-copy) | Mail sent through an Exchange (EWS) account is saved to Sent Items again when the server does not save a copy itself. | [#272](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/272) |
| [`fix/mime-alternative`](https://github.com/rferrali/stamp/compare/fork-base...fix/mime-alternative) | A mail with several versions of its text (plain, HTML, Gmail's AMP) shows just the best one instead of turning the extra ones into blank attachments, and a text part added after the body, such as a mail gateway's "external sender" banner, no longer replaces it. | [#274](https://gitlab.gnome.org/jbrummer/stamp/-/work_items/274) |
| [`local/fork`](https://github.com/rferrali/stamp/compare/fork-base...local/fork) | Fork-only, never meant for upstream: this README and `local-rebuild-mine.sh`. | — |

### Working on the fork

Each change gets its own branch, cut from `fork-base` and named `feature/…`, `fix/…`, or `local/…` for fork-only material:

```sh
git switch -c fix/something fork-base
# … commit …
./local-rebuild-mine.sh
```

`./local-rebuild-mine.sh` rebuilds `mine` from scratch, which also syncs it with upstream:

1. Fetches `upstream` and `origin`, and moves `fork-base` to the latest upstream `main`. A different base, such as a stable branch, can be set with `git config fork.base upstream/<branch>`.
2. Rebases every `feature/*`, `fix/*` and `local/*` branch onto `fork-base`. Commits upstream has already taken drop out. A branch left with no commits is skipped, and the script prints how to delete it.
3. Tags the old `mine` as `mine-backup-<date-time>`, resets `mine` to `fork-base`, and merges each remaining branch with `--no-ff`, `local/*` last. `git log --first-parent fork-base..mine` then shows one line per change.
4. Pushes nothing. At the end it prints the `git push --force-with-lease` command for `fork-base`, `mine` and the topic branches.

If a rebase conflicts, resolve it, run `git rebase --continue`, and run the script again. Don't commit directly on `mine`: the next rebuild throws those commits away.

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
