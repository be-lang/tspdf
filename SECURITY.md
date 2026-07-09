# Security Policy

tspdf parses untrusted input: every PDF, image, and font it opens may be
attacker-controlled, and the `serve` command exposes a local HTTP server. We
take memory-safety and denial-of-service issues seriously.

## Reporting a vulnerability

Please report security issues **privately**, not through public issues or pull
requests.

- Preferred: open a [GitHub private security advisory](../../security/advisories/new).
- Alternatively, email **beni@lbau.org** with details and, if possible, a
  reproducer file.

Please include the tspdf version (`tspdf --version`), the platform, and the
input that triggers the issue. We aim to acknowledge reports within a few days
and will keep you updated as we investigate and fix.

Please give us a reasonable opportunity to release a fix before any public
disclosure.

## Scope

Especially interested in:

- Memory-safety bugs in the parsers (`src/reader/`, `src/compress/`,
  `src/image/`, `src/font/`): out-of-bounds access, use-after-free, integer
  overflow leading to under-allocation.
- Denial of service from crafted input (unbounded memory or recursion, hangs).
- Issues in the `serve` web server: request handling, resource exhaustion, path
  traversal, or anything that lets a request escape the local-only intent.
- Cryptographic mistakes in the from-scratch `src/crypto/` implementations.

## Hardening in place

- The parsers are exercised by libFuzzer harnesses (`make fuzz`) under
  AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer.
- CI runs the full suite under those sanitizers and validates output against
  `qpdf --check` and `mutool`.
- The `serve` server is intended for local use only and rejects cross-origin /
  DNS-rebound requests to its API.

## A note on the crypto

tspdf implements MD5, RC4, AES, and the SHA-2 family from scratch to honor its
zero-dependency rule. These are used for PDF-format encryption/decryption
compatibility, not as a general-purpose cryptography library. Do not reuse them
for other security purposes. In particular they are not constant-time: AES uses
key-dependent table lookups, so cache-timing attacks by co-resident processes
are outside the threat model of a local file tool.
