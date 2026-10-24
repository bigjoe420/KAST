# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.1.0   | Yes       |
| < 0.1.0 | No        |

## Reporting a vulnerability

Please **do not** open a public issue for security problems.

Use GitHub's private vulnerability reporting: go to the repo's
**Security → Advisories → Report a vulnerability** and describe what you
found. Reports typically get a first response within a few days.

Firmware-relevant areas we care most about:

- WebSocket message handling from the local relay (chat text, commands)
- Kick chat strings and usernames from untrusted broadcast sources
- SD-card file parsing (audio clips, config read-back)
- NVS-stored credentials and settings integrity (Wi-Fi, relay tokens)

## Scope notes

KAST connects only to services you point it at (the local relay server and
OBS on your own network). Kick credentials live in the relay's environment,
never on the device. Vulnerabilities in third-party components (ESP-IDF, LVGL)
should be reported upstream, but tell us too so we can bump the dependency.
