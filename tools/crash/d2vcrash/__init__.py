# d2vcrash — local admin tool of the D2Vita crash reports (design section 6).
#
# Everything here runs on the maintainer's machine only: it holds the admin
# token and the X25519 private key that opens the sealed artifacts. Nothing in
# this package is shipped to a console or to Cloudflare.
__all__ = ["seal"]
