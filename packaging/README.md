# Packaging

Recipes for distro packages, both building from the v0.1.0 source tarball on GitHub.
`aur/PKGBUILD` is for the Arch User Repository (`makepkg -si` to build locally).
`homebrew/tspdf.rb` is a Homebrew formula (`brew install --formula ./tspdf.rb`).
Publishing to the AUR or a Homebrew tap is a maintainer step; these files are the input for it, and the sha256 pins the tagged tarball.

When pushing to the AUR, regenerate the metadata first and commit it alongside the PKGBUILD:

```bash
cd aur && makepkg --printsrcinfo > .SRCINFO
```
