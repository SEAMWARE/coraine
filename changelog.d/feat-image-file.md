### New

- `coraine-build --image-file`: `--image`'s image saved as `<dir>/image.tar.gz` too (gzipped `docker save`, the
  name inside), for a machine without a registry - `docker load -i image.tar.gz` or `podman load -i`.
