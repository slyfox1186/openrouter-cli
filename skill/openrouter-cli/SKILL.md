---
name: openrouter-cli
description: "Second opinion, critique or peer review from another AI model hosted on OpenRouter (Gemini, GPT, Grok, DeepSeek, Qwen, Llama, Mistral, Kimi, GLM, Claude and others) through the `openrouter-cli` command, optionally pinned to one provider. Use when the user asks for an outside opinion, wants a different model to check, critique or improve a plan, design, diff or code, names an OpenRouter-hosted model or provider (\"ask Gemini 4 Argon to check this\"), asks for a follow-up round with that model, or asks about OpenRouter model prices, spend, credits or account status. Every question to a model is a billed call that needs the user's approval first."
---

# openrouter-cli

`openrouter-cli` sends a question, plus any files you attach, to one OpenRouter model and prints
its answer. By default the question is sent as written (`plain`). For a critique or peer review,
pass `--role review`, which makes the model act as a reviewer of a plan or code.

## Billing

Every question is a billed call. Before one:

1. Resolve the model and read its prices with `openrouter-cli show <model>` (free).
2. Tell the user the model id, any provider you will pin with `-p`, how many calls you will
   make, and the published input, cached input and output prices per million tokens from that
   output (the endpoint row's prices when you pin a provider). For a worst-case cost, take the
   highest `INPUT` and `OUTPUT` among the endpoint rows (only the pinned rows when you pin a
   provider). Price the prompt at that input rate and the output budget the call sends (the
   `Output budget` line, or the `--max-output-tokens` value you pass, capped at `Max output`) at
   that output rate or the model's `Internal reasoning` price, whichever is higher, then add any
   `Request` fee. When `show` says some requests are billed at different catalog rates, no worst
   case can be computed from it; say so.
3. Wait for approval, unless the user already approved that exact call in this turn.

After the call, report the final `cost` line from stderr to the user. These commands are free
and need no approval: `find`, `show`, `save`, `remove`, `list`, `status`,
`account balance`, `account keys`, `account credits`, `account activity`, `spend`,
`spend --resolve`.

## Keys and sign-in

Never run `login`, `logout` or `auth set`, and never read, print, pipe or ask for a
key. Keys are the user's to manage. If a command exits `7`, run `openrouter-cli status` and tell
the user what it says; when it shows no stored key or a rejected key, ask them to run
`openrouter-cli login` (or `login --replace`) in their own terminal.

## Choosing the model and provider

- `openrouter-cli list` shows saved aliases. Check it first.
- `openrouter-cli find "gemini 4 argon"` lists matching ids. Quote multi-word names.
- `-m` accepts a saved alias, an exact id (`google/gemini-4-argon`), a `~author/family-latest`
  id, or plain words (`-m "gemini 4 argon"`). Exact IDs take precedence over aliases; a plain
  name that matches exactly one model is used without saving an alias.
- `openrouter-cli save ALIAS MODEL_ID` saves an alias by hand. A model keeps one alias: saving
  another name for the same id removes the old one.
- `openrouter-cli remove ALIAS` deletes a saved alias.
- These model commands also work with `models` in front (`openrouter-cli models save ...`).
- Exit `4` means the name is unknown or matches several models; the candidates are printed. Pick
  the exact id the user meant, or ask them.
- Pin a provider only when the user asks for one. `-p VALUE` (repeatable) is passed unchanged to
  OpenRouter as `provider.only`; copy each value from the `TAG` column of the endpoint table in
  `show`, or use a tag's provider prefix (`google-vertex`) for that provider's standard
  endpoints. Flex, ultrafast and closed-weight tier endpoints are opt-in: a base slug never
  selects them, so pass the full tag (for example `google-vertex/global/priority`). The `fast`
  and `priority` suffixes are interchangeable. OpenRouter decides what matches.
- If OpenRouter finds no endpoint for the request, the call exits `5` with a hint. Under the
  default `zdr` privacy the pinned provider may lack a ZDR endpoint; see Privacy below.

## Sending a question

Options come first and the query comes last:

```bash
openrouter-cli -m google/gemini-4-argon --role review -f src/http.cpp -f src/http.hpp \
  "Goal: retry free GET requests safely. Constraints: ... Review the retry logic."
```

- `-f FILE` (repeatable) attaches a file, sent line-numbered so the reviewer cites `path:line`.
- For a long prompt, pass `-` as the query and feed it on stdin with a quoted heredoc. One
  command-line argument is limited to about 128 KiB.

  ```bash
  openrouter-cli -m google/gemini-4-argon --role review -f plan.md - <<'EOF'
  Goal, constraints, what changed, what you want checked...
  EOF
  ```

- Put `--` before a query that starts with `-` or equals a subcommand word (`find`, `show`,
  `save`, `remove`, `list`, `models`, `auth`, `status`, `account`, `spend`, `login`, `logout`).
- The reviewer cannot read files, run commands or see this conversation. Send everything it
  needs: the user's goal, constraints, acceptance criteria, the plan or code, relevant excerpts of
  docs, and checks already run with their results.
- Never send secrets, `.env` files, credentials or private data unrelated to the question. The
  CLI sends text and files exactly as given and checks none of them for secrets; choosing safe
  context is your job.

## Roles and options

- `--role plain` (default) sends no review instructions: the model answers the question as
  written.
- `--role review` returns `Assumptions`, a `Verdict` (`APPROVE`, `APPROVE WITH CHANGES` or
  `RETHINK`), severity-ordered findings with location, evidence and exact fix, optional
  enhancements, and what was not verified. Pass it whenever the user wants a review or critique.
- `-s TEXT` or `-s @FILE` (long form `--system-prompt`) adds system prompt text.
- `-e EFFORT` sets reasoning effort (`max`, `xhigh`, `high`, `medium`, `low`, `minimal`, `none`);
  the default comes from the user's `config.env` (or legacy `.env`). Models that cannot select effort ignore it.
- `--max-output-tokens N` sets the output budget, reasoning included. Leave it unset: the default
  (the `Output budget` line of `show`) leaves room for long reasoning, and a smaller cap can cut
  the answer off after the call is billed. Pass it only when the user asks, or higher after an
  exit `6` whose message advises a higher `--max-output-tokens`.

## Follow-up rounds

`--session NAME` (letters, digits, `.`, `_`, `-`) keeps the history and pins the model and system
text from the first call. For the next round, reuse the same name without `-m`, `-s` or
`--role`, for example to rebut a finding or to ask the model to verify your fixes:

```bash
openrouter-cli -m google/gemini-4-argon --role review --session http-retry -f src/http.cpp "Review ..."
openrouter-cli --session http-retry -f src/http.cpp "I fixed findings 1 and 3; verify them."
```

Each round is another billed call and resends the whole history. Only complete answers are
added to a session.

## Long calls

Replies often take 3 to 25 minutes. Until the answer starts, stderr prints a progress line every
10 seconds for the first minute and every 30 seconds after; `no answer text yet` is normal while
a model reasons, because reasoning text is excluded. While answer text is arriving the lines
stop. Silence on stdout does not mean the call is stuck. The `asking` line shows the role in use
(`role from session` for a resumed session).

- Claude Code: run the command with `run_in_background: true` and an explicit long `timeout`.
  Unattended runs cap background commands at 30 minutes by default (10 for text `-p` runs) and
  up to 2 hours when a timeout is passed. Wait for the completion notice instead of polling.
- Codex: run it as a long-running command and keep polling until it exits.

stdout is the answer only. The answer is also saved to the file named at the end of the `cost`
line, so read that file if stdout was lost. When stdout and stderr are captured together (a
terminal, or `2>&1`), stderr adds a blank line above and below the answer to set it apart from
the `openrouter-cli:` lines.

## Exit codes

| Code | Meaning | What to do |
| --- | --- | --- |
| `0` | Success | Judge the answer (below) and report the cost line. |
| `1` | Internal error | Report the message to the user. |
| `2` | Usage error | Fix the command; `openrouter-cli --help`. |
| `3` | Refused before sending: input too large, session busy, cost log not writable | Nothing was sent or billed. Shrink the input, wait until the other call on that session ends, or report the cost log problem. |
| `4` | Unknown or ambiguous model | Choose from the printed candidates or ask the user. |
| `5` | API or network failure, rejected key, or no endpoint matching the model, providers and privacy | Report the message. For no endpoint, check tags with `show`. A retry is another billed call; ask first. |
| `6` | Incomplete answer (output limit reached, stream ended early, content filter) | The partial answer is in the answer file and the session is unchanged. For an output limit the message gives the reasoning and answer token counts when usage allows, and says whether a higher `--max-output-tokens` can help. Retry as it advises only after telling the user it costs another call. |
| `7` | Configuration problem: no stored key, key cannot be decrypted, plaintext key in settings, invalid settings | Follow Keys and sign-in above. Never read or print `config.env` or `.env`. |
| `8` | Interrupted, during the answer or during the cost lookup after it | The partial answer was kept; report the cost line. |
| `9` | The call finished and may have been billed, but saving failed (answer file, cost record or session) | Use the answer from stdout, report the message and cost line, and never retry automatically. |

When a cost line says `unknown`, run `openrouter-cli spend --resolve` later (free) and report
what it finds.

## Privacy

The default `--privacy zdr` routes only to endpoints that do not train on prompts and are listed
as zero data retention. Depending on the user's OpenRouter privacy settings, this can include
Safety Retention endpoints, where OpenRouter or the provider may keep prompts and responses.
Use `--privacy no-training` only for a model or provider that has no ZDR endpoint, and tell the
user before the call that the provider may retain the prompt.

## Judging the answer

Treat the reply as claims to check. Verify every finding against the code, tests and current
documentation before acting on it, fix the ones that hold, and tell the user which you rejected
and why. Agreement between models is not proof.
