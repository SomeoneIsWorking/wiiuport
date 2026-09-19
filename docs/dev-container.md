# Building without touching the host

The runtime needs 19 native development packages. Installing them on the host means a
privileged command someone has to run, which stops work whenever that person is away.
A Fedora **toolbox** container removes the need: it shares the same home directory and
the same user, so `~/repo` is the same checkout, but `sudo dnf install` inside it
affects only the container.

`toolbox` is part of a Fedora workstation install and needs nothing added. `distrobox`
does the same job if you prefer it; it is not installed here, and toolbox already works,
so nothing depends on which one is used.

## Setting it up

```sh
toolbox create --assumeyes wiiu
toolbox run -c wiiu sudo dnf install -y --setopt=install_weak_deps=False \
    bluez-libs-devel cairo-devel clang cmake freeglut-devel glm-devel gtk3-devel \
    libgcrypt-devel libsecret-devel libusb1-devel nasm ninja-build systemd-devel \
    wayland-protocols-devel libpng-static
```

Do not copy that package list into a script. It is the list
`tools/wiiuport/hostdeps.py` prints when something is missing, and that refusal is the
source of truth. Run the check and install exactly what it names:

```sh
toolbox run -c wiiu bash -lc 'cd ~/repo/wiiu/wiiuport && uv run --frozen python tools/build_runtime.py'
```

The build refuses by exact package name before compiling anything, so a missing
dependency costs one command rather than a failed build.

## What the container does and does not change

It changes where development packages are installed. It does not change the build,
which is the same `tools/build_runtime.py` with the same locked Python environment and
the same Clang and Ninja selection, and it is not a cross-compilation or isolation
boundary. A build inside the container and one on a host with the packages present
produce the same binary from the same tree.

It is not required. The host build works when the packages are present, and the CI
Linux job builds from a cold checkout with neither.

Game files are never copied into the container. The disc image stays wherever the
player keeps it and is passed by path, as it is on the host.
