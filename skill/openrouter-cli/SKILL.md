---
name: openrouter-cli
description: "Second opinion, critique or peer review from another AI model hosted on OpenRouter (Gemini, GPT, Grok, DeepSeek, Qwen, Llama, Mistral, Kimi, GLM, Claude and others) through the `openrouter-cli` command, optionally pinned to one provider. Use when the user asks for an outside opinion, wants a different model to check, critique or improve a plan, design, diff or code, names an OpenRouter-hosted model or provider (\"ask Gemini 4 Argon to check this\"), asks for a follow-up round with that model, or asks about OpenRouter model prices, spend, credits or account status. Every question to a model is a billed call that needs the user's approval first."
---

# openrouter-cli

`openrouter-cli` sends a question, plus any files you attach, to one OpenRouter model and prints
its answer. The default `review` role makes the model act as a peer reviewer of a plan or code.

## Billing

Every question is a billed call. Before one:

1. Resolve the model and read its prices with `openrouter-cli models show <model>` (free).
2. Tell the user the model id, any provider you will pin with `-p`, how many calls you will
   make, and the published input, cached input and output prices per million tokens from that
   output (the endpoint row's prices when you pin a provider).
3. Wait for approval, unless the user already approved that exact call in this turn.

After the call, report the final `cost` line from stderr to the user. These commands are free
and need no approval: `models find`, `models show`, `models save`, `models list`, `status`,
`account balance`, `account keys`, `account credits`, `account activity`, `spend`,
`spend --resolve`.

## Keys and sign-in

Never run `auth login`, `auth set` or `auth logout`, and never read, print, pipe or ask for a
key. Keys are the user's to manage. If a command exits `7`, run `openrouter-cli status` and tell
the user what it says; when it shows no stored key or a rejected key, ask them to run
`openrouter-cli auth login` (or `auth login --replace`) in their own terminal.

## Choosing the model and provider

- `openrouter-cli models list` shows saved aliases. Check it first.
- `openrouter-cli models find "gemini 4 argon"` lists matching ids. Quote multi-word names.
- `-m` accepts a saved alias, an exact id (`google/gemini-4-argon`), a `~author/family-latest`
  id, or plain words (`-m "gemini 4 argon"`). Exact IDs take precedence over aliases; a plain
  name that matches exactly one model is used without saving an alias.
- `openrouter-cli models save ALIAS MODEL_ID` saves an alias by hand.
- Exit `4` means the name is unknown or matches several models; the candidates are printed. Pick
  the exact id the user meant, or ask them.
- Pin a provider only when the user asks for one. `-p VALUE` (repeatable) is passed unchanged to
  OpenRouter as `provider.only`; copy each value from the `TAG` column of the endpoint table in
  `models show`, or use a tag's provider prefix (`google-vertex`) for that provider's standard
  endpoints. Flex, ultrafast and closed-weight tier endpoints are opt-in: a base slug never
  selects them, so pass the full tag (for example `google-vertex/global/priority`). The `fast`
  and `priority` suffixes are interchangeable. OpenRouter decides what matches.
- If OpenRouter finds no endpoint for the request, the call exits `5` with a hint. Under the
  default `zdr` privacy the pinned provider may lack a ZDR endpoint; see Privacy below.

## Sending a question

Options come first and the query comes last:

```bash
openrouter-cli -m google/gemini-4-argon -f src/http.cpp -f src/http.hpp \
  "Goal: retry free GET requests safely. Constraints: ... Review the retry logic."
```

- `-f FILE` (repeatable) attaches a file, sent line-numbered so the reviewer cites `path:line`.
- For a long prompt, pass `-` as the query and feed it on stdin with a quoted heredoc. One
  command-line argument is limited to about 128 KiB.

  ```bash
  openrouter-cli -m google/gemini-4-argon -f plan.md - <<'EOF'
  Goal, constraints, what changed, what you want checked...
  EOF
  ```

- Put `--` before a query that starts with `-` or equals a subcommand word (`models`, `auth`,
  `status`, `account`, `spend`).
- The reviewer cannot read files, run commands or see this conversation. Send everything it
  needs: the user's goal, constraints, acceptance criteria, the plan or code, relevant excerpts of
  docs, and checks already run with their results.
- Never send secrets, `.env` files, credentials or private data unrelated to the question. The
  CLI refuses known secret formats and credential files (exit `3`, nothing sent), but it cannot
  catch everything; choosing safe context is your job.

## Roles and options

- `--role review` (default) returns `Assumptions`, a `Verdict` (`APPROVE`,
  `APPROVE WITH CHANGES` or `RETHINK`), severity-ordered findings with location, evidence and
  exact fix, optional enhancements, and what was not verified.
- `--role plain` sends no review instructions; use it for a plain question.
- `-s TEXT` or `-s @FILE` adds system text.
- `-e EFFORT` sets reasoning effort (`max`, `xhigh`, `high`, `medium`, `low`, `minimal`, `none`);
  the default comes from the user's `config.env` (or legacy `.env`). Models that cannot select effort ignore it.
- `--max-output-tokens N` sets the output budget, reasoning included.

## Follow-up rounds

`--session NAME` (letters, digits, `.`, `_`, `-`) keeps the history and pins the model and system
text from the first call. For the next round, reuse the same name without `-m`, `-s` or
`--role`, for example to rebut a finding or to ask the model to verify your fixes:

```bash
openrouter-cli -m google/gemini-4-argon --session http-retry -f src/http.cpp "Review ..."
openrouter-cli --session http-retry -f src/http.cpp "I fixed findings 1 and 3; verify them."
```

Each round is another billed call and resends the whole history. Only complete answers are
added to a session.

## Long calls

Replies often take 3 to 25 minutes. stderr prints a heartbeat every 30 seconds; silence on stdout
does not mean the call is stuck.

- Claude Code: run the command with `run_in_background: true` and an explicit long `timeout`.
  Unattended runs cap background commands at 30 minutes by default (10 for text `-p` runs) and
  up to 2 hours when a timeout is passed. Wait for the completion notice instead of polling.
- Codex: run it as a long-running command and keep polling until it exits.

stdout is the answer only. The answer is also saved to the file named at the end of the `cost`
line, so read that file if stdout was lost.

## Exit codes

| Code | Meaning | What to do |
| --- | --- | --- |
| `0` | Success | Judge the answer (below) and report the cost line. |
| `1` | Internal error | Report the message to the user. |
| `2` | Usage error | Fix the command; `openrouter-cli --help`. |
| `3` | Refused before sending: secret found, input too large, session busy, credential file, cost log not writable | Nothing was sent or billed. Remove the flagged content, shrink the input, wait until the other call on that session ends, or report the cost log problem. |
| `4` | Unknown or ambiguous model | Choose from the printed candidates or ask the user. |
| `5` | API or network failure, rejected key, or no endpoint matching the model, providers and privacy | Report the message. For no endpoint, check tags with `models show`. A retry is another billed call; ask first. |
| `6` | Incomplete answer (budget ran out, stream ended early, content filter) | The partial answer is in the answer file and the session is unchanged. Retry with a lower `--effort` or a higher `--max-output-tokens` only after telling the user it costs another call. |
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
