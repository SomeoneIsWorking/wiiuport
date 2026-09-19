# wiiuport

A Wii U runtime for Linux desktop, built from a maintained fork of
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0), exposed to consuming title
projects as a library rather than as a standalone application.

Its reason to exist beyond upstream Cemu is a title-neutral mechanism for **replaying a
frame's guest draw stream with substituted render state**, so a consuming title can
present interpolated frames between two simulation ticks without changing the guest's
tick rate.

- Epic intent: [`docs/project-goals.md`](docs/project-goals.md)
- What actually works today: [`docs/project-state.md`](docs/project-state.md)
- Who owns what: [`docs/codemap.md`](docs/codemap.md)

The upstream fork is pinned as the `external/cemu` submodule. This repository contains
no game files, keys, or anything derived from them, and never will.

First consumer: [`setsail`](../setsail) — The Legend of Zelda: The Wind Waker HD.
