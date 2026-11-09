# Session archive

`omp-session-public-sanitized.zip` was generated with the
[`omp-archive`](https://github.com/alosslessdev/omp-archive-main) plugin.

## Contents

| File | Contents |
|---|---|
| `manifest.json` | Archive index and counts |
| `session.jsonl` | Main session transcript |
| `agents.json` | Agent metadata observed by the host |
| `jobs.json` | Background/async job index |
| `viewer.html` | Standalone HTML viewer |
| `README.md` | Plugin-generated index |

Two archives are kept here:

| File | Contents |
|---|---|
| `omp-session-public-sanitized.zip` | no-artifacts export (transcript, agents, jobs, viewer) |
| `omp-session-full-sanitized.zip` | full export, including untruncated tool logs and the session artifact tree |

## Sanitization

The raw transcript contained one live Google API key in a shell environment
dump. It was replaced with `[REDACTED_GOOGLE_API_KEY]` before publication.
Local usernames, home paths, host names, volume UUIDs and the author's email
addresses were also redacted. Other credential patterns were scanned and none
were found.

The archive can still contain shell history and technical details. Review it
before redistributing.
