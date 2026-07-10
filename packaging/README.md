# Packaging

`aur/PKGBUILD` builds from the tagged source tarball on GitHub for the Arch
User Repository (`makepkg -si` to build locally). Publishing to the AUR is a
maintainer step; the sha256 pins the tagged tarball.

When pushing to the AUR, regenerate the metadata first and commit it alongside
the PKGBUILD:

```bash
cd aur && makepkg --printsrcinfo > .SRCINFO
```
