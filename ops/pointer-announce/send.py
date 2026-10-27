#!/usr/bin/env python3
"""Announce Backpack repo pointers over Logos Delivery (backpack ADR 0007).

The pointer files are the signed envelopes `backpack publish` uploads to /srv/data/{public,unlisted}/backpack/
<repo key>.json. Each is sent verbatim to /backpack/1/<repo key>/json through a logosctl session's
delivery_module, where phones (the Loam shared node) receive it live and fleet Store keeps it for catch-up.

Runs as root (the logosctl session is root's), so after the 2026-10-23 audit (BP-2) it trusts nothing
in the directory:
  - only regular files, never symlinks; opened with O_NOFOLLOW;
  - the file name must be a repo key (43 base64url chars) + ".json"; at most 4 KiB;
  - the content must be a pointer envelope whose ed25519 signature verifies with the key in the file
    name, and whose body names that same key. Anything else is skipped and reported, never sent;
  - logosctl is called without a shell.

Usage: send.py [KEY ...]   (no args: every pointer file; the timer does that twice a day so Store keeps them)
"""
import base64, json, os, re, stat, subprocess, sys, time

CTL = os.environ.get("BACKPACK_CTL", "/root/scala-hub2/ctl.sh")
DIRS = os.environ.get("BACKPACK_DIRS", "/srv/data/public/backpack:/srv/data/unlisted/backpack").split(":")
KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
MAX = 4096

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
except ImportError:
    sys.exit("send.py needs python3-cryptography for ed25519")


def b64u(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def read_pointer(d, name):
    """Return the file's text if it is a valid signed pointer for the key in its name, else raise."""
    key = name[:-5]
    if not name.endswith(".json") or not KEY_RE.match(key):
        raise ValueError("not a repo-key (or read-key id) file name")
    path = os.path.join(d, name)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise ValueError("not a regular file")
        if st.st_nlink != 1:  # a hardlink to another file (audit R-10); the signature check would refuse it anyway
            raise ValueError("has other hard links")
        if st.st_size > MAX:
            raise ValueError(f"{st.st_size} bytes, over {MAX}")
        raw = os.read(fd, MAX + 1)
    finally:
        os.close(fd)
    text = raw.decode("utf-8")
    env = json.loads(text)
    if env.get("backpack") != 1 or env.get("kind") != "pointer":
        raise ValueError("not a pointer envelope")
    body, sig = env.get("body"), env.get("sig")
    if not isinstance(body, str) or not isinstance(sig, str):
        raise ValueError("missing body or signature")
    # A private repo's pointer (ADR 0009) is encrypted and named after its read key's id, not the repo key;
    # it carries the repo key in the clear so this check still works without the read key.
    enc = env.get("enc")
    signer = key
    if enc is not None:
        if enc != "a256gcm":
            raise ValueError(f"unknown encryption {enc!r}")
        signer = env.get("key")
        if not isinstance(signer, str) or not KEY_RE.match(signer):
            raise ValueError("encrypted pointer without a repo key")
    try:
        Ed25519PublicKey.from_public_bytes(b64u(signer)).verify(b64u(sig), body.encode("utf-8"))
    except (InvalidSignature, ValueError) as e:
        raise ValueError(f"signature does not verify with {signer[:6]}…") from e
    if enc is None and json.loads(body).get("key") != key:
        raise ValueError("body names a different repo key")
    return key, text


def send(key, text):
    r = subprocess.run([CTL, "call", "delivery_module", "send", f"str:/backpack/1/{key}/json", f"str:{text}"],
                       capture_output=True, text=True, timeout=60)
    return (r.stdout or r.stderr).strip()[:200]


def main(keys):
    now = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    sent = 0
    for d in DIRS:
        try:
            names = sorted(e.name for e in os.scandir(d) if e.is_file(follow_symlinks=False))
        except FileNotFoundError:
            continue
        for name in names:
            if keys and name[:-5] not in keys:
                continue
            try:
                key, text = read_pointer(d, name)
            except Exception as e:  # skipped, never sent
                print(f"{now} SKIP {d}/{name[:60]}: {e}")
                continue
            print(f"{now} {key} {send(key, text)}")
            sent += 1
    return 0


if __name__ == "__main__":
    sys.exit(main(set(sys.argv[1:])))
