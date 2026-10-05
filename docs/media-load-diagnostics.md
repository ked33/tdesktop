# Media loading diagnostics

Media loading diagnostics are written to the normal `log.txt` automatically.
They do not require the verbose playback or MTProto debug logging settings.
Search for `Media load:` and `Media load summary:`.

The diagnostics observe loading; they do not change scheduling, retry delays,
cache contents, or automatic download settings.

## Volume and overhead

- One coarse timer per account checks at 15-second intervals while tracking work.
- A load is considered stalled after at least 15 seconds without progress.
- Each stalled load is reported at most once per 60 seconds.
- All accounts share a limit of 6 detail lines per 15-second window and
  24 detail lines per 60-second window, plus 4 summary lines per 60-second window.
- Successful files and progressing streams are sampled by DC, resource kind,
  and streaming/non-streaming mode, at most once per minute per group.
  Completion of a previously reported stalled load is also eligible for a
  detail line, within the same global limits.
- Each account tracks at most 2,048 loads; overflow is counted as `untracked`.
- `suppressed` counts omitted detail lines. Summaries contain interval totals,
  not one line per downloaded fragment. Idle loaders are removed from tracking.

## Reading a detail line

`scope` identifies an account's download manager without logging the account ID.
`id` is an in-process diagnostic identifier. A normal MTProto file loader uses
the same identifier for its cache and network stages. `request` is an RPC
request identifier that can be matched to existing RPC error log entries.

| Field | Meaning |
| --- | --- |
| `kind`, `tag`, `auto`, `stream` | Resource kind, cache tag, automatic-file flag, streaming flag |
| `stage` | Cache lookup, decode queued/running, main-thread delivery, queued, request, receiving, reference refresh, or retry wait |
| `age_ms`, `idle_ms` | Load age and time since progress; terminal records use the finish time |
| `first_send_ms`, `first_byte_ms` | Delay from load creation; `-1` means no network dispatch/data observed |
| `cache_ms` | Time through the local cache/decode callback; `-1` means it has not returned |
| `pending`, `deferred`, `oldest_ms` | Outstanding request counts and oldest dispatch age |
| `refs`, `refreshed`, `ref_wait_ms`, `ref_ms` | Reference refresh attempts, successful refreshes, current oldest wait, latest successful duration |
| `retries`, `retry_ms`, `error` | Scheduled RPC retries, remaining deadline, and bounded error type |
| `queue` | Current DC capacity, ready tasks, highest priority group, and representative head load |

`gate=priority` means a lower-priority task is ready but the currently selected
priority group has no ready task. Other gates include `server_wait`,
`smart_limit`, `session_capacity`, `deferred_pending`, and `no_ready_task`.
These are snapshots at reporting time, not proof that the condition persisted
for the entire stall. `rate_timer_ms` reports the manager's scheduling timer;
it does not force a pacing check or send a request.

A retry deadline reaching zero does not prove the retry reached the server.
Compare the same load's subsequent progress/completion and RPC errors.
Buffered video with no outstanding requests is not classified as stalled.

For a reproduction, retain the log through failure and partial recovery, then
compare a failed new image/thumbnail with a successful stream on the same DC.
The new records contain no message text, URLs, access hashes, file references,
proxy configuration, or authentication data.
