# openrouter-cli

`openrouter-cli` sends a question and optional source files to any model on
[OpenRouter](https://openrouter.ai) and streams the answer to standard output. It was written so
coding agents such as Claude Code and Codex can ask another model to review a plan or a change,
but it works the same from a shell. Each call prints the charge OpenRouter reports for it, and
every call is recorded in a local cost log.

## Requirements

- Linux on x86_64.
- CMake 4.0 or later and Ninja.
- Clang 23 with lld (default presets), or GCC 15 (the `gcc` preset), with a standard library
  that supports C++26 (GCC 15's libstdc++).
- Static libraries for OpenSSL 3, zlib and zstd (`libssl.a`, `libcrypto.a`, `libz.a`,
  `libzstd.a`). On Ubuntu they come with `libssl-dev`, `zlib1g-dev` and `libzstd-dev`.
- `clang-format`, `clang-tidy` and `cppcheck` for `scripts/check.sh`.
- Network access to github.com during the first configure, which downloads nlohmann/json,
  CLI11, jitterentropy and curl (plus doctest and cpp-httplib for tests) from release archives
  pinned by SHA-256.
- Network access to openrouter.ai at run time, and an OpenRouter API key.

## Build

```bash
scripts/check.sh
```

`check.sh` runs, in order: a clang-format check, a debug build with AddressSanitizer and
UndefinedBehaviorSanitizer and its tests, clang-tidy, cppcheck, the release build, and a check
that the release binary links no shared library except glibc. Every step runs even when an
earlier one fails, so one run reports everything. The full output is saved to
`build/logs/check-<UTC time>.log`, with `build/logs/latest.log` pointing to the newest one.

`scripts/check.sh --gcc` also builds and tests with GCC as a portability check.

The binary is `build/release/openrouter-cli`. To build only the release binary:

```bash
cmake --preset release
cmake --build --preset release
```

## Install

```bash
scripts/install.sh
```

`install.sh` does not build. It creates these symbolic links to the already verified build:

| Link | Target |
| --- | --- |
| `~/.local/bin/openrouter-cli` | `build/release/openrouter-cli` |
| `~/.agents/skills/openrouter-cli` | `skill/openrouter-cli` (Codex user skills) |
| `~/.claude/skills/openrouter-cli` | `../../.agents/skills/openrouter-cli` (Claude Code) |

It refuses to replace a real file or directory, or a link that points somewhere else. Because
these are links, a later rebuild takes effect without running it again. `~/.local/bin` must be on
your `PATH`.

The skill in `skill/openrouter-cli/SKILL.md` tells an agent when to use the tool, to get your
approval with the model's published prices before each billed call, and how to send context and
read the result.

## Configuration

```bash
cp .env.example .env
chmod 600 .env
```

Then set `OPENROUTER_API_KEY` in `.env`. A key with its own spend limit, created at
<https://openrouter.ai/settings/keys>, keeps the tool's spending separate from other uses.

The project `.env` is the only configuration source; exported shell variables do not override it.
The binary has the project directory compiled in, so it finds `.env` and `state/` from any
working directory. Set `OPENROUTER_CLI_HOME` to use a different project directory. The CLI exits
with code 7 if `.env` is missing, is not a regular file owned by you, has any group or other
permission bit set, or has no key.

| Setting | Default | Meaning |
| --- | --- | --- |
| `OPENROUTER_API_KEY` | none | Required. |
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
data retention. `no-training` sends only `data_collection = "deny"`; use it for a model that has
no ZDR endpoint, accepting that the provider may keep the prompt.

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
openrouter-cli spend [--since today|7d|30d|all]
openrouter-cli balance
```

Options come first and the query comes last. Put `--` before a query that starts with `-` or is a
subcommand name. Question options cannot be combined with a subcommand.

| Option | Meaning |
| --- | --- |
| `-m, --model` | Saved alias, exact id, `~author/family-latest`, or plain words. |
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
`state/catalog.json`; `--refresh` refetches it. `models show` prints the id, context length,
maximum output, published prices per million tokens for input, cached input, cache writes and
output, supported reasoning efforts and any expiration date. Prices are catalog rates with the
time they were fetched; the actual charge can differ by provider.

When `-m` gets plain words that match exactly one model, that model is used and the name is saved
as an alias. Several matches or none exit with code 4 and print the candidates.

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

A session pins its model and system text when it is created; later rounds omit `-m`, `-s` and `--role`.
Only complete answers are added to the history, and the whole history is sent each round. A
session is locked while a call uses it; a second call on the same session exits with code 3.

### Spend and balance

```bash
openrouter-cli spend --since 7d
openrouter-cli balance
```

`spend` totals the local cost log by UTC day and by model, and lists calls whose cost is still
unknown. `balance` shows the key's usage and limit from OpenRouter. Both are free.

## Output

Standard output carries only the answer, streamed as it arrives. Standard error carries the
resolved model, a progress line every 30 seconds, any error, and a final cost line:

```text
openrouter-cli: cost $0.0412 | google/gemini-4-argon via Google Vertex | in 18,204 (cached 0) | out 3,911 (reasoning 2,870) | answer /path/to/state/answers/<call_id>.md
```

The cost is the account charge OpenRouter returns for the call; the tool never computes it from
prices. If the stream did not include it, the tool asks OpenRouter's generation endpoint for up
to about 30 seconds. Values that are still unknown print as `unknown`, never as 0.

The answer file is created before the request is sent and written as text arrives, so a partial
answer survives an error or an interruption.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Complete answer. |
| 1 | Internal error. |
| 2 | Usage error. |
| 3 | Refused before sending: possible secret, credential file, input too large, session busy, or cost log not writable. Nothing was sent. |
| 4 | Unknown or ambiguous model; candidates are printed. |
| 5 | API or network failure. |
| 6 | Incomplete answer: output budget used up, stream ended early, content filter, or empty reply. |
| 7 | Configuration error. |
| 8 | Interrupted by SIGINT, SIGTERM or SIGHUP; the partial answer is kept. |

The generation request is never retried automatically, since a retry is billed again. Free
requests (catalog, generation lookup, balance) are retried up to 3 times on connection errors,
408, 429 and 5xx responses.

## Where state lives

Project state is in `state/` inside the project directory (ignored by git; directories mode 700,
files mode 600):

| Path | Contents |
| --- | --- |
| `state/catalog.json` | Cached model catalog. |
| `state/models.json` | Saved aliases. |
| `state/answers/<call_id>.md` | One file per call with the answer text. |
| `state/sessions/<name>.json` | Session history, model and system text. |
| `state/locks/` | Lock files for sessions and the alias registry. |

The cost log is global for the user: `~/.local/state/openrouter-cli/costs.jsonl`, or
`$XDG_STATE_HOME/openrouter-cli/costs.jsonl` when that variable is set. Each call writes a
`pending` record before the request (if that write fails, nothing is sent), a `generation`
record once OpenRouter assigns an id, a `result` record at the end, and a `resolved` record if
the cost had to be looked up afterwards. `spend` reports malformed lines instead of skipping
them. A call killed before OpenRouter assigned its generation id stays unresolved in the log;
its charge still appears in OpenRouter's activity page.

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

The tool never prints or logs the API key, request bodies, prompt text or provider error bodies.
Provider errors are shown as a status, an error type and a shortened message with anything that
looks like a secret removed. Prompts and answers are stored locally in `state/` as plain text.

Requests turn off OpenRouter's web search and context compression plugins. Plugins you enable as
account defaults on OpenRouter may still run, and their charges are included in the reported
cost.

## Linking and portability

The release binary links libcurl, OpenSSL, zlib, zstd, libstdc++ and libgcc statically. Only
glibc (`libc`, `libm` and the dynamic loader) is shared, because a statically linked glibc cannot
resolve host names reliably on other systems. `scripts/check.sh` fails if the binary needs any
other shared library.

Release builds use `-march=native` by default, so the binary runs only on CPUs with the same
instruction set extensions as the build machine. For a binary that runs on any x86-64 CPU,
configure with the option off:

```bash
cmake --preset release -DORCLI_NATIVE=OFF
cmake --build --preset release
```
