# openrouter-cli

`openrouter-cli` sends a question and optional source files to any model on
[OpenRouter](https://openrouter.ai), optionally pinned to specific providers, and streams the
answer to standard output. It was written so coding agents such as Claude Code and Codex can ask
another model to review a plan or a change, but it works the same from a shell. Each call prints
the charge OpenRouter reports for it, and every call is recorded in a local cost log. API keys
are stored encrypted with `systemd-creds`, never in plain text.

## Requirements

- Linux on x86_64.
- CMake 4.0 or later and Ninja.
- Clang 19 or later (`clang` and `clang++`). lld is used when that Clang finds `ld.lld`;
  otherwise the default linker is used.
- GCC 15's libstdc++, the C++ standard library Clang uses on Linux, for C++26 library support.
- Static libraries for OpenSSL 3, zlib and zstd (`libssl.a`, `libcrypto.a`, `libz.a`,
  `libzstd.a`). On Ubuntu they come with `libssl-dev`, `zlib1g-dev` and `libzstd-dev`.
- `clang-format`, `clang-tidy` and `cppcheck` for the checks (a `clang-format-N` or
  `clang-tidy-N` matching the Clang major version is preferred when present).
- Network access to github.com during the first configure, which downloads nlohmann/json,
  CLI11, jitterentropy and curl (plus doctest and cpp-httplib for tests) from release archives
  pinned by SHA-256.
- At run time: systemd with `systemd-creds --user` (tested with systemd 259), network access to
  openrouter.ai, and an OpenRouter account.

## Build and install

```bash
scripts/build.sh
```

The script always builds with the newest `clang` and `clang++` pair on `PATH` (named `clang`
or `clang-N`); it stops if none is found or the newest is older than Clang 19. A default run
performs these steps, reporting each as ok or FAILED:

1. Format check with clang-format.
2. Debug build with AddressSanitizer and UndefinedBehaviorSanitizer, then the tests.
3. clang-tidy and cppcheck on `src/`.
4. Release build (`-O2 -march=native`, link-time optimization, hardening flags, static
   libstdc++ and libgcc).
5. Static-link check: the binary may need only glibc (`libc`, `libm`, the dynamic loader and
   the vDSO), then `--version`.
6. Install, only if every earlier step passed.

Later steps still run after a failure, so one log shows everything. The log is saved to
`build/logs/build-<UTC time>.log`, with `build/logs/latest.log` pointing to the newest one, and
ends with a summary of failed steps. The script exits 0 only if every step it ran passed.

| Option | Effect |
| --- | --- |
| `--force` | Overwrite an existing, different `openrouter-cli` in the bin directory, and re-point skill links that point elsewhere. |
| `--bin-dir DIR` | Install the binary to `DIR` (default `~/.local/bin`). |
| `--no-skill` | Install only the binary. |
| `--no-install` | Run the checks and build; install nothing. |
| `--quick` | Skip the format check, debug build, tests, clang-tidy and cppcheck: release build, static-link check and install only. |
| `--fresh` | Reconfigure every build directory from scratch. |
| `--jobs N` | Parallel build jobs (default: `nproc`). |
| `--list-compilers` | Show every Clang found and mark the one that would be used, then exit. |
| `-h`, `--help` | Show the options. |

Each Clang version gets its own build directories, `build/debug-clang-<version>` and
`build/release-clang-<version>`; clang-tidy and cppcheck read the debug tree's compile database.
Invalid options exit with code 2.

### What gets installed

| Path | What |
| --- | --- |
| `~/.local/bin/openrouter-cli` | A copy of the release binary, mode 755. |
| `~/.agents/skills/openrouter-cli` | Link to `skill/openrouter-cli` (Codex user skills). |
| `~/.claude/skills/openrouter-cli` | Link to `../../.agents/skills/openrouter-cli` (Claude Code). |

The binary is copied atomically. An identical binary already in place is left as is; a link into
this project from an older install is replaced; any other file is refused unless `--force` is
given; a directory is always refused. The same goes for the skill links, except that a real file
or directory there is always refused. `~/.local/bin` must be on your `PATH`.

The installed binary is a standalone copy, so you can move it. It keeps the project directory
compiled in and reads `.env` and `state/` from there; set `OPENROUTER_CLI_HOME` to use another
project directory.

To remove the install (the skill paths are links, so this leaves `skill/` untouched):

```bash
rm ~/.local/bin/openrouter-cli ~/.agents/skills/openrouter-cli ~/.claude/skills/openrouter-cli
```

Stored keys stay in `state/credentials/`; run `openrouter-cli auth logout` first to delete them.

The skill in `skill/openrouter-cli/SKILL.md` tells an agent when to use the tool, to get your
approval with the model's published prices before each billed call, and how to send context and
read the result.

## Signing in

```bash
openrouter-cli auth login
```

`auth login` uses OpenRouter's OAuth sign-in. It prints a URL; open it in any browser, sign in
with your OpenRouter email and password on openrouter.ai, approve, and paste the code the page
shows back into the terminal. The code is valid for 10 minutes and works once. OpenRouter then creates an
API key labelled `openrouter-cli on HOST` in your account, which the CLI checks and stores
encrypted. The CLI never asks for or sees your password. Because only a URL and a code change
hands, this works over SSH: open the URL on any computer.

To store a key you already have instead:

```bash
openrouter-cli auth set
```

`auth set` reads the key from the terminal with echo off, or from standard input when it is
piped. The key must start with `sk-or-`. Before storing it, the CLI checks it with OpenRouter;
an inference key that turns out to be a Management key is refused.

If a key is already stored, both commands refuse unless `--replace` is given. A key that fails
the check is not stored and leaves any existing one in place. If `auth login` created a key but
could not store it, the message names the key's label so you can revoke it at
<https://openrouter.ai/settings/keys>.

### Management key

OpenRouter only lets a Management key read the account's keys, credits and activity; a key from
`auth login` cannot. Create one at <https://openrouter.ai/settings/management-keys> (an expiry
date is a good idea), then store it:

```bash
openrouter-cli auth set --management
```

A Management key is used only for the `account keys`, `account credits` and `account activity`
commands and for the identity lines of `status`. It is never sent with a question or to `/key`.

### Status and logout

```bash
openrouter-cli status
openrouter-cli auth logout
openrouter-cli auth logout --management
```

`status` prints one `name: value` line per item and never prints key text:

- `credentials`: where keys are stored (`systemd-creds --user on HOST`).
- `logged in`: `yes`, `no` (with a hint to run `auth login`), `key rejected` (with a hint to run
  `auth login --replace`) or `unknown` when the lookup failed.
- From the stored API key: `key creator` (your user id, or the member who created an
  organization key), `owner` (`organization <id>` or `individual account`), `workspace`,
  `key label` (OpenRouter's masked form, such as `sk-or-v1-0e6...1c96`), `key expires`,
  `free tier`, `usage today`, `usage this week`, `usage this month`, `usage total`, `limit`,
  `limit remaining` and `limit reset`.
- `management key`: `stored`, `not stored` (with a hint to add one), or `stored but rejected`
  (with a hint to run `auth set --management --replace`).
- With a Management key: `credits remaining (Management key's account)`, then the key
  creator's `email`, `name` and `role`, each marked "as seen by the Management key's
  organization". The two keys are stored separately and need not belong to the same account.

Values OpenRouter leaves empty show as `n/a`; `email` says `not available` when OpenRouter has no
match or no access, and `lookup failed` when the search could not finish. `status` exits 5 when a
stored key is rejected or a lookup fails, and 7 when a stored key cannot be decrypted.

`auth logout` deletes every stored key; `--management` deletes only the Management key. It works
without `systemd-creds` and with damaged credential files, and logging out twice is not an
error. Logout only removes the local copy: the key stays valid at OpenRouter until you revoke it
at <https://openrouter.ai/settings/keys>.

## Key storage

Keys are encrypted with `systemd-creds --user` and stored in
`state/credentials/api-key.cred` and `state/credentials/management-key.cred` (directory mode 700,
files mode 600). systemd chooses the encryption key: its host secret, plus the TPM2 chip when one
is available. A `--user` credential is also bound to your user id, user name and machine id, and
each file carries its own name, so the two files cannot be swapped. The first use makes systemd
create its host secret, `/var/lib/systemd/credential.secret`, owned by root.

Credentials are bound to the machine. Each computer signs in on its own with `auth login` or
`auth set`; copying `.cred` files to another machine does not work.

What this protects against: disclosure of a credential file on its own, for example when it is
copied, backed up, committed to git or read by another account. What it does not protect
against: code running as your user or as root, which can decrypt the key the same way the CLI
does, or someone holding the whole disk including the host secret. The CLI keeps no plaintext
key in files, command-line arguments, environment variables or logs; it clears its own copies of
a key from memory after use and disables core dumps so a crash cannot write a key to disk. It
makes no claim about swap or about copies inside the libraries it uses.

There is no plaintext fallback. If `systemd-creds` is missing or fails, every command that needs
a key exits with code 7 and says why.

### Moving a key out of `.env`

Older versions read `OPENROUTER_API_KEY` from `.env`. A non-empty `OPENROUTER_API_KEY` or
`OPENROUTER_MANAGEMENT_KEY` line in `.env` now makes every command exit with code 7, except
`auth set`, `auth login` and `auth logout`. Move the key once, without printing it, then delete
the line from `.env`:

```bash
grep -m1 '^OPENROUTER_API_KEY=' .env | cut -d= -f2- | openrouter-cli auth set
```

## Configuration

The optional project `.env` holds settings only, never keys. Without it every setting takes the
default below. To change one:

```bash
cp .env.example .env
chmod 600 .env
```

The project `.env` is the only configuration source; exported shell variables do not override it.
If `.env` exists, it must be a regular file owned by you with no group or other permission bits,
or commands exit with code 7.

| Setting | Default | Meaning |
| --- | --- | --- |
| `OPENROUTER_CLI_BASE_URL` | `https://openrouter.ai/api/v1` | API root. |
| `OPENROUTER_CLI_APP_REFERER` | empty | `HTTP-Referer` header; empty sends none. |
| `OPENROUTER_CLI_APP_TITLE` | `openrouter-cli` | App name sent in the `X-OpenRouter-Title` header. |
| `OPENROUTER_CLI_APP_CATEGORIES` | `cli-agent` | Sent in the `X-OpenRouter-Categories` header. |
| `OPENROUTER_CLI_DEFAULT_EFFORT` | `high` | Reasoning effort when `--effort` is not given. |
| `OPENROUTER_CLI_MAX_OUTPUT_TOKENS` | `64000` | Output budget, reasoning included; clamped to each model's limit. |
| `OPENROUTER_CLI_PRIVACY` | `zdr` | `zdr` or `no-training` (see below). |
| `OPENROUTER_CLI_CATALOG_TTL_HOURS` | `24` | Age at which the cached model catalog is refetched. |
| `OPENROUTER_CLI_IDLE_TIMEOUT_SECONDS` | `300` | Abort when no bytes arrive for this long; keepalives count. |
| `OPENROUTER_CLI_TOTAL_TIMEOUT_SECONDS` | `3600` | Upper limit for one call. |
| `OPENROUTER_CLI_MAX_FILE_BYTES` | `1048576` | Largest file accepted by `-f`. |
| `OPENROUTER_CLI_MAX_INPUT_BYTES` | `4194304` | Largest total request text, history included. |

### Privacy setting

With `zdr`, every request carries `provider.data_collection = "deny"` and `provider.zdr = true`,
so OpenRouter routes it only to endpoints that do not train on prompts and are listed as zero
data retention. `no-training` sends only `data_collection = "deny"`; use it for a model or
provider that has no ZDR endpoint, accepting that the provider may keep the prompt.

`zdr = true` also admits Safety Retention endpoints when your account allows them. Under Safety
Retention, OpenRouter or the provider may keep prompts and responses from any request, flagged or
not. Self-serve accounts allow Safety Retention unless you change it; to exclude those endpoints,
choose "ZDR only" in your OpenRouter privacy settings (Google models have a separate setting).
Some models are then unavailable. See OpenRouter's
[zero data retention guide](https://openrouter.ai/docs/guides/features/zdr).

## Usage

```text
openrouter-cli [OPTIONS] [--] QUERY            QUERY "-" reads the prompt from stdin
openrouter-cli models find [--refresh] [--] NAME
openrouter-cli models show [--] MODEL
openrouter-cli models save ALIAS MODEL_ID
openrouter-cli models list
openrouter-cli auth login [--replace]
openrouter-cli auth set [--management] [--replace]
openrouter-cli auth logout [--management]
openrouter-cli status
openrouter-cli account balance
openrouter-cli account keys
openrouter-cli account credits
openrouter-cli account activity [--days N]
openrouter-cli spend [--since today|7d|30d|all] [--resolve]
```

Options come first and the query comes last. Put `--` before a query that starts with `-` or is a
subcommand name. Question options cannot be combined with a subcommand.

| Option | Meaning |
| --- | --- |
| `-m, --model` | Saved alias, exact id, `~author/family-latest`, or plain words. |
| `-p, --provider` | Use only this provider slug or endpoint tag; repeatable. |
| `-f, --file` | File to include, line-numbered; repeatable. |
| `--role` | `review` (default) or `plain`. |
| `-s, --system` | Extra system text, or `@FILE` to read it from a file. |
| `-e, --effort` | `max`, `xhigh`, `high`, `medium`, `low`, `minimal` or `none`. |
| `--max-output-tokens` | Output budget for this call. |
| `--privacy` | `zdr` (default) or `no-training`. |
| `--session` | Keep history under this name for follow-up questions. |

### Finding a model

```bash
openrouter-cli models find "gemini 4 argon"
openrouter-cli models show google/gemini-4-argon
openrouter-cli models save argon google/gemini-4-argon
openrouter-cli models list
```

`models find` and `models show` read OpenRouter's public model catalog, cached in
`state/catalog.json`; `--refresh` refetches it. These commands need no key. `models show` prints
the id, name, context length, maximum output, reasoning support, published prices per million
tokens for input, cached input, cache writes, output and internal reasoning, any per-request
price and any expiration date. A table of the model's endpoints follows, with the columns
`TAG`, `PROVIDER`, `INPUT`, `OUTPUT`, `CONTEXT`, `MAX OUTPUT` and `NOTES` (service tier and
endpoint status). Prices are catalog rates with the time they were fetched; the serving
provider's rate can differ.

When `-m` gets plain words that match exactly one model, that model is used and the name is saved
as an alias. Several matches or none exit with code 4 and print the candidates.

### Choosing a provider

```bash
openrouter-cli -m google/gemini-4-argon -p google-vertex "..."
openrouter-cli -m google/gemini-4-argon -p google-vertex/global/priority "..."
```

Each `-p` value is sent unchanged in OpenRouter's `provider.only` list; the order is not a
preference. Copy values from the `TAG` column of `models show`, or use a tag's provider prefix,
such as `google-vertex`, for that provider's standard endpoints. OpenRouter applies its own
routing rules:

- A service-tier endpoint's slug is its standard endpoint's slug with the tier appended, such as
  `google-vertex/global/priority`.
- Flex, ultrafast and closed-weight tier endpoints are opt-in: a base slug never selects them,
  so pass the full tag. Priority endpoints of open-weight models take part in default routing
  and do match their base slug.
- The `fast` and `priority` slug suffixes are interchangeable, so `openai/priority` matches the
  same endpoint as `openai/fast`.
- Your account's allowed-provider settings also apply.

See OpenRouter's
[provider selection guide](https://openrouter.ai/docs/guides/routing/provider-selection) and
[service tiers guide](https://openrouter.ai/docs/guides/features/service-tiers).

When no endpoint satisfies the providers, the privacy setting and your account settings,
OpenRouter rejects the request and the CLI exits with code 5, suggesting
`openrouter-cli models show MODEL`. Under the default `zdr` privacy, a provider without a ZDR
endpoint is rejected this way; `--privacy no-training` allows it.

### Asking for a review

```bash
openrouter-cli -m argon -f src/http.cpp -f src/http.hpp \
  "Goal: retry free GET requests on 429 and 5xx. Review the retry logic."
```

Files are read once and sent with their paths and line numbers, so the reply can cite
`src/http.cpp:120`. The `review` role asks for assumptions, a verdict (`APPROVE`,
`APPROVE WITH CHANGES` or `RETHINK`) and findings ordered by severity, each with a location,
evidence and an exact fix. `--role plain` sends the question alone.

A single command-line argument is limited to about 128 KiB, so send long prompts through stdin
with a quoted heredoc:

```bash
openrouter-cli -m argon -f docs/plan.md - <<'EOF'
Goal and constraints...
Questions for the reviewer...
EOF
```

### Follow-up rounds

```bash
openrouter-cli -m argon --session retry -f src/http.cpp "Review the retry logic."
openrouter-cli --session retry -f src/http.cpp "I fixed findings 1 and 3. Check the fixes."
```

A session pins its model and system text when it is created; later rounds omit `-m`, `-s` and
`--role`. Only complete answers are added to the history, and the whole history is sent each
round. A session is locked while a call uses it; a second call on the same session exits with
code 3.

### Account information

```bash
openrouter-cli account balance
openrouter-cli account keys
openrouter-cli account credits
openrouter-cli account activity --days 7
```

All four only read from OpenRouter and are free.

- `account balance` uses the stored API key and shows its usage and limit.
- `account keys` lists the API keys in the Management key's workspace (an organization-scoped
  Management key lists its default workspace), disabled and expired keys included: label,
  created and expiry dates, disabled flag, usage per day, week, month and total, limit and
  remaining. Key secrets are never shown; OpenRouter does not return them.
- `account credits` shows credits purchased, used and remaining.
- `account activity` shows requests, prompt, completion and reasoning tokens and cost per UTC
  date, model and provider for the last 30 completed UTC days. `--days N` (1 to 30) limits it
  to the last N completed days.

`account keys`, `account credits` and `account activity` need a Management key.

### Spend

```bash
openrouter-cli spend --since 7d
openrouter-cli spend --resolve
```

`spend` totals the local cost log by UTC day and by model, and lists calls whose cost is still
unknown. `spend --resolve` instead asks OpenRouter for the charge of every unknown-cost call that
has a generation id, oldest first (it ignores `--since`). It prints each charge it records and a
count of calls resolved, still unknown and failed; run `spend` afterwards for the totals. A
resolved call is never looked up again. Ctrl-C stops between lookups, keeping every charge
already recorded. `--resolve` exits 5 if any lookup failed and 9 if a charge could not be
written to the cost log. Both forms are free.

## Output

Standard output carries only the answer, streamed as it arrives. Standard error carries the
resolved model, a progress line every 30 seconds, any error, and a final cost line:

```text
openrouter-cli: cost $0.0412 | google/gemini-4-argon via Google Vertex | in 18,204 (cached 0) | out 3,911 (reasoning 2,870) | answer /path/to/state/answers/<call_id>.md
```

The cost is the account charge OpenRouter returns for the call; the tool never computes it from
prices. If the stream ended without it, the tool asks OpenRouter's generation endpoint for up to
about 30 seconds. After an idle timeout, a network error or an interruption the generation may
still be running, so the cost stays unknown and `spend --resolve` can fetch it later. Unknown
values print as `unknown`, never as 0.

The answer file is created before the request is sent and written as text arrives, so a partial
answer survives an error or an interruption.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success. |
| 1 | Internal error. |
| 2 | Usage error. |
| 3 | Refused before sending: possible secret, credential file, input too large, session busy, cost log not writable, or a key already stored without `--replace`. Nothing was sent. |
| 4 | Unknown or ambiguous model; candidates are printed. |
| 5 | API or network failure, including a rejected key and no endpoint matching the request. |
| 6 | Incomplete answer: output budget used up, stream ended early, content filter, or empty reply. |
| 7 | Configuration error: invalid `.env`, plaintext key in `.env`, no stored key, or `systemd-creds` missing or failing. |
| 8 | Interrupted by SIGINT, SIGTERM or SIGHUP; the partial answer is kept. Also used when the answer finished but Ctrl-C stopped the cost lookup; `spend --resolve` can fetch the cost later. |
| 9 | The call finished and may have been billed, but saving failed: the answer file, a cost record, or the session. The message names what failed and says not to retry automatically; the answer still went to standard output and any known cost is printed. `spend --resolve` also exits 9 when it cannot write a found charge. |

When several apply, 8, 5 and 6 take precedence over 9. The question request is never retried
automatically, since a retry is billed again; after exit 9 in particular, do not repeat the call
without checking the cost line and `spend`. Free requests (catalog, generation lookup, account
reads) are retried up to 3 times on connection errors, 408, 429 and 5xx responses.

## Where state lives

Project state is in `state/` inside the project directory (ignored by git; directories mode 700,
files mode 600):

| Path | Contents |
| --- | --- |
| `state/catalog.json` | Cached model catalog. |
| `state/models.json` | Saved aliases. |
| `state/credentials/` | Encrypted API key and Management key. |
| `state/answers/<call_id>.md` | One file per call with the answer text. |
| `state/sessions/<name>.json` | Session history, model and system text. |
| `state/locks/` | Lock files for sessions, credentials and the alias registry. |

The cost log is global for the user: `~/.local/state/openrouter-cli/costs.jsonl`, or
`$XDG_STATE_HOME/openrouter-cli/costs.jsonl` when that variable is set. Each call writes a
`pending` record before the request (if that write fails, nothing is sent), a `generation`
record once OpenRouter assigns an id, a `result` record at the end, and a `resolved` record if
the cost had to be looked up afterwards. `spend` reports malformed lines instead of skipping
them. A call killed before OpenRouter assigned its generation id stays unresolved in the log;
its charge still appears in `account activity` and on OpenRouter's activity page.

## Privacy and secret scanning

Everything you send goes to OpenRouter and to the provider that serves the model. Retention
depends on the privacy setting above and on the provider policies OpenRouter publishes; this tool
cannot verify them.

Before sending, the CLI scans the query, system text (including `-s @FILE`), session history and
every attached file for common credential formats: vendor API keys, private-key blocks,
credential assignments and URLs with embedded passwords. It also refuses credential files by
their resolved path, such as `.env`, private keys and SSH or cloud credential files
(`.env.example` and `.env.sample` are allowed). A match stops the call with exit code 3 and names
the file, line and pattern, never the value. The scan catches only the formats it knows, so it
can miss a secret and can flag harmless text. You remain responsible for what you send.

The tool never prints or logs a key, request bodies, prompt text or provider error bodies.
Provider errors are shown as a status, an error type and a shortened message with anything that
looks like a secret removed. Prompts and answers are stored locally in `state/` as plain text.

Requests turn off OpenRouter's web search and context compression plugins. Plugins you enable as
account defaults on OpenRouter may still run, and their charges are included in the reported
cost.

## Linking and portability

The release binary links libcurl, OpenSSL, zlib, zstd, libstdc++ and libgcc statically. Only
glibc (`libc`, `libm` and the dynamic loader) is shared, because a statically linked glibc cannot
resolve host names reliably on other systems. The build fails its static-link check if the
binary needs any other shared library.

Release builds use `-march=native`, so the binary runs only on CPUs with the same instruction
set extensions as the build machine. `scripts/build.sh` has no option to change this; for a
binary that runs on any x86-64 CPU, configure a separate release tree by hand:

```bash
cmake --preset release -B build/release-portable -DORCLI_NATIVE=OFF \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/release-portable
```
