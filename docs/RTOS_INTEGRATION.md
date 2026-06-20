# RTOS Integration

## Mutex rule

Protect one `nvlog_ctx_t` with one mutex.

## ISR rule

Do not call `nvlog_*` from ISR context.

## Blocking

All backends may block on I/O, erase, or transport.

## Watchdog

If your platform needs a watchdog kick during erase or long writes, do that in the backend, not in the core.

## Task model

Recommended:

- one logger task
- one queue of events
- one serialized nvlog context

