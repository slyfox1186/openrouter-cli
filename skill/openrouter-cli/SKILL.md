---
name: openrouter-cli
description: "Use when seeking another LLM's advice on project decisions, asking an outside model to compare approaches or review a plan, design, diff or code, naming an OpenRouter model or provider (for example Gemini, DeepSeek, Kimi, Grok or Qwen), saying to call or ask the NAME LLMs, models or group (\"call the coding LLMs\"), using openrouter-cli for a question or follow-up, or checking OpenRouter prices, spend, credits or account status."
---

# openrouter-cli

Alongside the `codex` and `claude` CLIs, `openrouter-cli` provides another way to consult LLMs
when making project decisions. It sends a question, plus any files you attach, to any chat model
available through OpenRouter, or with `-g` to every model in a saved group at once, and prints
the answer. Model selection is open; saved aliases and the examples below are not a fixed list of
supported models.

Use it to compare approaches, investigate a difficult bug, or get another model's assessment of
a plan or code. It supplements any required `codex`/`claude` peer review. By default the question
is sent as written (`plain`); pass `--role review` for a critique or peer review.

## Billing

Every question is a billed call. Before one:

1. Resolve the model and read its prices with `openrouter-cli show <model>` (free).
2. Tell the user the model id, the providers the call will use, how many calls you will make,
   and the published input, cached input and output prices per million tokens from that output
   (the endpoint rows' prices when the call is restricted to providers). The providers are the
   `:TAG` or `-p` you pass; without those, the `Saved provider` line of `show`; with
   `--any-provider`, any endpoint. For a worst-case cost, take the highest `INPUT` and `OUTPUT`
   among the endpoint rows those providers allow (a tag's provider prefix, such as
   `google-vertex`, allows its standard rows, not `flex` or `priority` ones). Price the prompt
   at that input rate and the output budget the call sends (the `Output budget` line, or the
   `--max-output-tokens` value you pass, capped at `Max output`) at that output rate or the
   model's `Internal reasoning` price, whichever is higher, then add any `Request` fee. When
   `show` says some requests are billed at different catalog rates, no worst case can be
   computed from it; say so and obtain approval rather than relying on a spending cap.
3. Obtain approval unless the user already approved that exact call in this turn or an explicit
   standing policy authorizes it. Apply the user's total-cost limits and required funds checks to
   every call, including individually approved calls, follow-ups and retries. If required funds
   cannot be read or are insufficient, stop and report the missing figure or shortfall; never
   reduce the requested work or output budget to fit the balance.

After the call, report the final `cost` line from stderr to the user. These commands are free
and need no approval: `find`, `show`, `save`, `remove`, `list`, `pin`, `unpin`, `group add`,
`group remove`, `group list`, `status`, `account balance`, `account keys`, `account credits`,
`account activity`, `spend`, `spend --resolve`.

## Keys and sign-in

Never run `login`, `logout` or `auth set`, and never read, print, pipe or ask for a
key. Keys are the user's to manage. If a command exits `7` with a key or setting
problem, run `openrouter-cli status` and tell the user what it says. When it shows no stored key,
or a rejected key on a second `status` run as well (OpenRouter can reject a good key once), ask
them to run `openrouter-cli login` (or `login --replace`) in their own terminal. When the message names a corrupt `models.json`, `providers.json` or `groups.json`
instead, report the file and entry it names and ask the user how to fix it; never delete or edit
it yourself. While `providers.json` is corrupt, a question that passes `:TAG`, `-p` or
`--any-provider` still works.

## Choosing the model and provider

- `openrouter-cli list` shows saved aliases. Check it first.
- `openrouter-cli find "gemini 4 argon"` lists matching ids. Quote multi-word names.
- `-m` accepts a saved alias, an exact id (`google/gemini-4-argon`), a `~author/family-latest`
  id, or plain words (`-m "gemini 4 argon"`). Exact IDs take precedence over aliases; a plain
  name that matches exactly one model is used without saving an alias.
- Examples include `deepseek/deepseek-v4.1-flash-1017` and `moonshotai/kimi-k3.1-pro`. Use
  `find` and `show` to check current availability and prices for these or any other model;
  endpoint availability and the privacy/provider filters still apply.
- `openrouter-cli save ALIAS MODEL_ID` saves an alias by hand. A model keeps one alias: saving
  another name for the same id removes the old one.
- `openrouter-cli remove ALIAS` deletes a saved alias.
- These model commands also work with `models` in front (`openrouter-cli models save ...`).
- Exit `4` means the name is unknown or matches several models; the candidates are printed. Pick
  the exact id the user meant, or ask them.
- Restrict the providers only when the user asks. Put the tag after the model with a colon
  (`-m google/gemini-4-argon:google-vertex/global`), or pass `-p TAG` (repeatable, for several
  providers); never both in one call. Either is passed unchanged to OpenRouter as
  `provider.only` for that call only. Copy each tag from the `TAG` column of the endpoint table
  in `show`, or use a tag's provider prefix (`google-vertex`) for that provider's standard
  endpoints. Flex, ultrafast and closed-weight tier endpoints are opt-in: a base slug never
  selects them, so pass the full tag (for example `google-vertex/global/priority`). The `fast`
  and `priority` suffixes are interchangeable. OpenRouter decides what matches.
- The tag is the text after the last colon. An exact id that has a colon (`author/model:free`)
  is used whole. The variant words `:nitro`, `:floor`, `:exacto`, `:online`, `:thinking`,
  `:extended` and `:batch` are refused (exit `4`), and any other word after the last colon is
  read as a tag, so a typo there becomes a provider; use `-p` for a provider whose tag is a
  variant word.
- `openrouter-cli pin MODEL TAG...` saves providers for a model, and `openrouter-cli unpin MODEL`
  clears them. Run them only when the user asks to save or clear a provider. A saved provider
  applies to every later question to that exact model id, under any name and in every session
  round, until a `:TAG` or `-p` replaces it for one call or `--any-provider` ignores it for one
  call (privacy settings still apply). `list` prints every saved provider and `show` prints the
  model's `Saved provider` line; check them before pricing a call.
- If OpenRouter finds no endpoint for the request, the call exits `5` with a hint. Under the
  default `zdr` privacy the pinned provider may lack a ZDR endpoint; see Privacy below.

## Sending a question

Options come first and the query comes last. After the billing checks, choose the command that
fits the task; these are independent examples, not a requirement to call both models:

```bash
openrouter-cli -m deepseek/deepseek-v4.1-flash-1017 -f plan.md \
  "Compare the approaches in this plan against its goals and constraints. Recommend one and explain the tradeoffs."

openrouter-cli -m moonshotai/kimi-k3.1-pro --role review -f src/http.cpp -f src/http.hpp \
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
  `save`, `remove`, `list`, `pin`, `unpin`, `group`, `models`, `auth`, `status`, `account`,
  `spend`, `login`, `logout`).
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
  the default comes from the user's `config.env` (or legacy `.env`). Models that cannot select
  effort ignore it.
  Pass `-e xhigh` on every question unless the user names another level for that call
  (the user's standing rule, 2026-11-11); never lower it by your own judgment.
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
added to a session. A session keeps its model, not a `:TAG` or `-p`: pass it again in each round,
or suggest `pin` when every round should use that provider.

## Groups of models

When the user says to call or ask a named set of models ("call the coding LLMs", "ask the
reviewers group"):

1. Run `openrouter-cli group list` (free). If no group has that name, ask the user which models
   to use; create a group with `openrouter-cli group add NAME "MODEL,MODEL"` only when they ask.
2. Run `openrouter-cli show` on each member's model part (the text before a member's `:TAG`).
   It resolves the current concrete id and shows its saved provider; a member's own `:TAG`
   overrides that.
3. Price each member as in Billing (its providers' rows, any session history, the output
   budget), and give the user each member's worst case and the total. A group question is one
   billed call per member. The group's existence is not approval.
4. Run `openrouter-cli -g NAME [options] QUERY` in the background with a long timeout, like any
   long call. `-g` cannot be combined with `-m` or `-p`; the other question options apply to every
   member, and `--any-provider` is refused when a member has its own `:TAG`.
5. stdout holds one block per member, each starting with a line
   `===== MEMBER (CONCRETE_ID) =====` (only `===== MEMBER =====` when the member is that exact
   id) and printed when that member finishes. Every stderr line
   from a member starts with `[MEMBER]`, and the last lines summarize each member (complete,
   incomplete, failed with its exit code, interrupted, never started). Judge each answer on its
   own and report every member's `cost` line.
6. The exit code is `8` when interrupted, `1` when a member ended unexpectedly, `0` when all
   completed, and otherwise the first failed member's code; it never describes the whole
   group's billing. Never rerun the whole group after a partial failure or an incomplete answer:
   ask the user, then retry only the affected members with `-m CONCRETE_ID[:TAG]`, and resolve
   unknown costs with `spend --resolve`.

Follow-up rounds: `--session NAME` with `-g` keeps one history per member. The next round repeats
`-g NAME --session NAME` and drops `--role` and `-s`. Once a group conversation has started,
every member it asks must already have history; after members change, use a new session name.

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
| `4` | Unknown or ambiguous model, an unknown group or member, or `unpin` found no saved provider | Choose from the printed candidates or ask the user. `list` and `group list` show what is saved. |
| `5` | API or network failure, rejected key, or no endpoint matching the model, providers and privacy | Report the message. For no endpoint, check tags with `show`, including a saved provider; the user can clear it with `unpin` or skip it with `--any-provider`. A retry is another billed call; repeat the Billing checks first. |
| `6` | Incomplete answer (output limit reached, stream ended early, content filter) | The partial answer is in the answer file and the session is unchanged. For an output limit the message gives the reasoning and answer token counts when usage allows, and says whether a higher `--max-output-tokens` can help. A retry is another billed call; repeat the Billing checks with the new output budget before retrying as advised. |
| `7` | Configuration problem: no stored key, key cannot be decrypted, plaintext key in settings, invalid settings, or a corrupt `models.json`, `providers.json` or `groups.json` | Follow Keys and sign-in above. Never read or print `config.env` or `.env`. |
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
