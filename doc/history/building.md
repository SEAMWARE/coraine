# Building from source - history

What the build used to do, kept for whoever wants it. How to build today is in
[Building from source](../building.md) and [the details](../building-details.md). Newest first.

## Before 2026-10-02 - the builtin server's loops only did I/O

Before the coroutines (`doc/history/coroutines.md`), the description of `builtin` read:

> `builtin` selects **corHttp**, a sibling repo: an HTTP/1.1 server on
> edge-triggered epoll loops, depending on nothing but corAlloc and libc. Requests
> still run on corRest's worker pool — the loops only do I/O — and the wire
> format it emits is byte-for-byte the one libmicrohttpd produced, because several
> hundred functional tests compare captured responses line by line.
>
> It runs **several loops sharing one port**, each with its own `accept()` on its
> own `SO_REUSEPORT` socket and its own work queue, so the kernel spreads incoming
> connections across them and no loop hands work to another.

With a request running on the loop that read it, the kernel's split of the connections became the
split of the work, and uneven; one accepting loop (`corHttpAcceptShare`) replaced it
(`doc/history/coroutines.md` § 9.1).

## MQTT notifications move to a plugin

The same held for the optional runtime deps. MQTT notifications were ~2 KB of
broker code against a `libmosquitto` that every build linked and every process
mapped, whether or not a single MQTT notification was ever sent. They are now
the `mqtt.so` bridge plugin: a broker started without `--bridges mqtt` never
maps libmosquitto.
