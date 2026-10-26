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

This is the **no-artifacts** export. The full archive includes untruncated tool
logs and the session artifact tree; it is retained locally and is not published.

## Sanitization

The raw transcript contained one live Google API key in a shell environment
dump. It was replaced with `[REDACTED_GOOGLE_API_KEY]` before publication.
Other credential patterns were scanned and none were found.

The archive can still contain local paths, shell history and technical details.
Review it before redistributing.
