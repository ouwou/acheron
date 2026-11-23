# drops the vcpkg archives this build didn't install and prints a cache key that only
# changes when the kept set does. vcpkg names each archive by the sha256 of the
# package's vcpkg_abi_info.txt
import hashlib
import pathlib
import sys

installed, archives, prefix = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]

used = {hashlib.sha256(f.read_bytes()).hexdigest() for f in installed.glob("*/share/*/vcpkg_abi_info.txt")}
if not used:
    sys.exit(f"nothing installed under {installed}, not touching the archives")

for archive in archives.glob("*/*.zip"):
    if archive.stem not in used:
        archive.unlink()

kept = sorted(archive.stem for archive in archives.glob("*/*.zip"))
print(f"kept {len(kept)} of {len(used)} installed packages", file=sys.stderr)
print(f"key={prefix}{hashlib.sha256(' '.join(kept).encode()).hexdigest()[:16]}")
