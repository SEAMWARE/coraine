### New

- `coraine-build --image <name>`: the build as a container image too - this distribution's base image, the packages
  of the libraries it links (the ones built from source copied), laid out as the published image (`/usr/local/bin`,
  `/opt/seamware`, `coraine-start`, `coraine-health`), so it runs where the published one does. 67 MiB with the
  default bridges on Ubuntu 26.04. docker or podman (`CONTAINER_ENGINE`); checked before anything is compiled.
