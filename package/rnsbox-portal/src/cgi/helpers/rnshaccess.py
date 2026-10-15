#!/usr/bin/env python3
# rnshaccess.py — client-key issuance for the box's rnsh remote shell.
#
# Called by the portal CGI (routes_access.cpp) as:
#   /usr/bin/python3 /usr/lib/rnsbox/rnshaccess.py issue
#
# `issue` mints a FRESH Reticulum Identity for an rnsh client and prints,
# between marker lines, the material the portal needs to (a) add the client's
# public hash to the rnsh allow-list and (b) build the downloadable access file:
#
#   RNSBOX-ACCESS/8
#   HASH=<truncated identity hash, 32 lowercase hex chars>
#   KEYB64=<base64 of the 64-byte private key>
#   DEST=<destination hash of the rnsh server, 32 lowercase hex chars>
#   RNSBOX-ACCESS-END
#
# Design constraints (local_docs/rnsh-access.md):
#   * The client private key exists ONCE — in this process's memory, then in
#     the HTTP response body. Nothing about it is ever written to disk. The
#     only key material on the box is the SERVER identity (the very file
#     S86rnsh uses), which is created here if missing so the destination hash
#     in the access file is correct even before the first S86rnsh start.
#   * No Reticulum transport is started: identity/destination math is pure
#     computation, so the helper works with rnsd down. Logging is silenced;
#     anything the helper does print besides the marker block is portal log
#     noise, never the payload — that is what the markers are for.
import base64
import os
import sys

SERVER_ID = "/etc/rnsbox/rnsh_identity"
APP_NAME = "rnsh"
MARK = "RNSBOX-ACCESS/8"
END = "RNSBOX-ACCESS-END"


def fail(msg):
    print("rnshaccess: " + msg, file=sys.stderr)
    sys.exit(1)


def issue():
    import RNS
    RNS.loglevel = RNS.LOG_NONE

    # The server identity is shared with S86rnsh (-i /etc/rnsbox/rnsh_identity).
    # Create it exactly once, atomically, root-only; S86rnsh reuses it.
    if not (os.path.isfile(SERVER_ID) and os.path.getsize(SERVER_ID) > 0):
        uid = RNS.Identity()
        try:
            fd = os.open(SERVER_ID, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as f:
                f.write(uid.get_private_key())
        except FileExistsError:
            pass  # a concurrent issuer won the race; use theirs below
        except OSError as e:
            fail("cannot write " + SERVER_ID + ": " + str(e))

    server = RNS.Identity.from_file(SERVER_ID)
    if server is None:
        fail(SERVER_ID + " exists but is not a valid Reticulum identity")

    dest = RNS.Destination(server, RNS.Destination.IN, RNS.Destination.SINGLE, APP_NAME)
    client = RNS.Identity()
    prv = client.get_private_key()
    if not prv or len(prv) != 64:
        fail("identity provider returned an unexpected private key size")

    print(MARK)
    print("HASH=" + client.hash.hex())
    print("KEYB64=" + base64.b64encode(prv).decode("ascii"))
    print("DEST=" + dest.hash.hex())
    print(END)


def main():
    if len(sys.argv) != 2 or sys.argv[1] != "issue":
        fail("usage: rnshaccess.py issue")
    if os.geteuid() != 0:
        fail("must run as root (the portal CGI does)")
    issue()


if __name__ == "__main__":
    main()
