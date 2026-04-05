# CS39006 Mini Project 2 – SimpleMail

**Name:** Jay Jani \
**Roll No.:** 23CS10027 \
**Github Repo. Link:** https://github.com/jayjani0011/CS39006_Mini_Project_2

---

## Overview

SimpleMail is a command-line email system built in C using TCP sockets. It implements two custom text-based protocols over TCP: **SMTP2** (for sending mail) and **SMP** (for retrieving mail). The system has three programs: `init`, `smserver`, and `smclient`.

---

## Building and Running

Makefile targets :
```bash
make runinit    # runs ./init users.txt to set up mailboxes
make runserver  # runs ./smserver 8080 users.txt
make runclient  # runs ./smclient 127.0.0.1 8080
make drunserver # runs ./dserver 8080 users.txt, i.e. server in debug mode to write debug.log
make drunclient # runs ./dclient 127.0.0.1 8080, i.e. client in debug mode to write debug.log
make clean      # removes all binaries
make deepclean  # removes binaries AND the mailboxes/ directory
```

Running :
```bash
make runinit
make runserver
make runclient
```

---

## Components

### `init.c` – Mailbox Initializer

Run once before starting the server. It reads `users.txt`, creates the `mailboxes/` directory, and for each valid user creates `mailboxes/<username>/` with a `0.txt` file that stores the next available unique mail ID (initialized to `0`). It is idempotent: existing directories and `0.txt` files are not overwritten, so running it again after mail has been delivered is safe.

### `smserver.c` – The Server

**Startup:** Reads `users.txt` into memory, validates each entry (lowercase-only usernames ≤ 20 chars, alphanumeric passwords ≤ 30 chars), and loads existing mailbox state (mail headers and counts) from disk into an in-memory `usinfo` array. The mailbox directories must already exist (created by `init`); the server exits with an error if they don't.

**Concurrency:** The server uses a single-process `select()`-based event loop to handle up to `MAX_CLIENTS` (10) concurrent connections without threads or forking. A `cliinfo` array tracks each connected client's file descriptor, state machine position, current in-progress message, authentication nonce, and remaining auth attempts.

**Protocol dispatch:** On each new connection the server sends `WELCOME SimpleMail v1.0\r\n`. The first command from the client must be `MODE SEND` or `MODE RECV`. The client's `state` field (an enum) drives strict sequence enforcement — any out-of-order command gets `ERR Bad sequence\r\n` and the connection is closed.

**SMTP2 (sending):** No authentication required. The state machine enforces `MODE SEND → FROM → TO (one or more) → SUB → BODY → (body lines) → dot → delivery → (new FROM or QUIT)`. On the dot terminator, the server writes one mail file per accepted recipient at `mailboxes/<username>/<unique_id>.txt` and also updates the in-memory inbox cache and `0.txt`. Dot-stuffing (stripping a leading `.` from body lines) is applied on receipt.

**SMP (retrieval):** After `MODE RECV`, the server sends a random 8-character alphanumeric nonce and requires the client to authenticate using a DJB2 hash of `password+nonce`. The client gets at most 3 attempts; after 3 failures the connection is closed. Supported commands post-authentication: `LIST`, `READ <id>`, `DELETE <id>`, `COUNT`, `QUIT`. `READ` applies dot-stuffing when sending file contents. `DELETE` removes the file from disk and shifts the in-memory inbox array.

**Logging:** Every significant event (connections, mode selection, auth success/failure, mail delivery, reads, deletes, disconnections) is printed to stdout with a `[YYYY-MM-DD HH:MM:SS]` timestamp. `SIGINT` is caught for a graceful shutdown that sends `BYE` to all connected clients before exiting.

### `smclient.c` – The Client

Connects to the server, receives the greeting, and presents a three-option main menu (Send / Mailbox / Quit). After each send or mailbox session, the client sends `QUIT`, closes the socket, and reconnects fresh for the next operation (since the server closes the connection after each `QUIT`).

- **Send (SMTP2):** Prompts for sender name, recipients (loops until blank line with at least one accepted), subject, and body (dot-terminated). Per-recipient feedback is shown inline. The raw protocol is hidden from the user.
- **Receive (SMP):** Prompts for username and password, computes the DJB2 hash locally (raw password is never sent), and authenticates. On success, presents a mailbox sub-menu: list, read, delete, logout. The `LIST` response is parsed using `~` as a field delimiter (see assumption below) and displayed in a formatted table.

---

## Mailbox File ID Management

Each user's `mailboxes/<username>/0.txt` stores the last-used integer ID. On every new mail delivery, the server increments this counter, writes the new mail as `<new_id>.txt`, and updates `0.txt`. IDs are **never reused** — deletion decrements `inboxCnt` but never touches `unique_id`. On server startup, `unique_id` is re-read from `0.txt` for each user, so the guarantee holds across restarts.

---

## Assumptions and Design Choices

1. **`~` as LIST field delimiter.** The spec says fields in `LIST` responses are tab-separated, but since both `From` (display name) and `Subject` can contain spaces, the server uses `~` as the delimiter instead. The client parses accordingly with `strtok`. This is noted here as it is a deviation from the spec's tab-separator suggestion.

2. **Body line limit.** The in-memory message buffer holds up to 50 body lines of up to 100 characters each. Bodies exceeding this are silently truncated in memory; however, delivery to disk still works correctly as the file is written line-by-line as they arrive. For the purposes of this project this limit is considered sufficient.

3. **Max clients fixed at 10.** Both the `select()` client array and the `usinfo` user registry are sized at `MAX_CLIENTS = 10`. This caps the number of simultaneous connections and registered users.

4. **`init` must be run before `smserver`.** The server does not create mailbox directories itself — it exits with an error if a user's directory is missing. Run `make runinit` once before the first `make runserver`.

5. **`ERR Authentication failed` includes remaining attempts.** The server appends the remaining attempt count to the authentication failure response (e.g., `ERR Authentication failed 2`) so the client can display it to the user. This is an extension of the base spec response.

6. **Empty subject handling.** If the client sends `SUB ` with nothing after the space, the subject is stored as an empty string, not `(no subject)`. This is a minor deviation from the spec.

7. **Nonce generation.** The nonce is generated using `rand()` seeded with `time(NULL) + i` (where `i` is the character index). This is not cryptographically secure but is sufficient for this project.

8. **`users.txt` requires at least one valid user.** Malformed lines are skipped silently, but if the file is entirely invalid or missing, the server exits.

9. **Debug mode.** Compiling with `-DDEBUG` (targets `dserver` / `dclient`) logs all raw protocol lines to `debug.log` for inspection.