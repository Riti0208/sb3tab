# Security Policy

## Supported versions

sb3tab is a hobby project with rolling development on the `main` branch.
Security fixes are applied to `main`; there are no maintained backport
branches.

## Reporting a vulnerability

If you find a vulnerability — particularly anything that could:

- run arbitrary code via a crafted `.sb3` project, asset, or QR payload
- exfiltrate stored WiFi credentials from `/sd/wifi.txt`
- cause persistent device state corruption through the QR / USB / WiFi paths
- bypass the project sandbox in any other unintended way

please report it privately rather than opening a public issue.

**Preferred channel:** open a [GitHub Security Advisory](https://github.com/Riti0208/sb3tab/security/advisories/new)
through the repository's *Security* tab. This keeps the discussion private
until a fix is ready.

**Alternative:** open a regular issue marked *security* with as little
reproducer detail as possible, and we'll move the conversation to a private
advisory.

## What to include

- Affected version (commit SHA or release tag)
- Target hardware
- Steps to reproduce or a minimal proof of concept
- Impact assessment (RCE, info disclosure, DoS, etc.)

## What to expect

sb3tab is maintained on a best-effort basis. You can expect:

- Acknowledgement within ~7 days
- A patch in `main` once the fix is verified
- Public disclosure after the fix lands, with credit (unless you prefer to
  remain anonymous)

There is no monetary bounty.
