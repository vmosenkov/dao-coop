# Using Codex as a subagent (Lead: DSH / DeepSeek)

Codex CLI 0.160.1 is installed and signed in with the owner's ChatGPT account.
It reads `AGENTS.md` from the working directory, so the project rules live
there and a task brief stays short. Codex is used as a **senior engineering
subagent**, never as the architect: the Lead (DeepSeek Harness agent) decides
the architecture, splits the work, reviews every diff and owns the result.

## Why the two long prompts from elsewhere were shortened

The drafts we were given (a Lead prompt and a Codex prompt) are correct in
substance — host-authoritative, don't reimplement DAO's combat, verify APIs,
state clearly what was tested — but as *per-call* prompts they are too long and
repeat each other. Codex re-reads `AGENTS.md` on every run, so the durable
rules live there; a call carries only the task. The Lead prompt is this file
plus the project's `docs/`. Nothing from the drafts was dropped that matters:
the fixed rules are in `AGENTS.md`, the validation vocabulary and report
format are there too.

## How to call Codex

Research (safe, makes no changes):

```powershell
codex exec -C <repo> -s read-only --skip-git-repo-check --color never `
  "Investigate <exact question>. Do not modify files. Report with file:line references."
```

Implementation of one small, agreed change (the Lead reviews the diff
afterwards; Codex never deploys to the game and never runs the game):

```powershell
codex exec -C <repo> -s workspace-write --skip-git-repo-check --color never `
  "Implement exactly <one deliverable>. <constraints>. Build with tools\build.ps1 -Preset debug before reporting."
```

A review pass (the Lead captured the diff first):

```powershell
git diff > E:\Temp_files\review.diff
codex exec -C <repo> -s read-only --skip-git-repo-check --color never `
  "Review E:\Temp_files\review.diff as a C++/networking reviewer. Look for lifetime bugs, threading violations, duplicate execution, desync, unvalidated network input. Do not modify files."
```

Rules for every call:

- **One deliverable per call.** Not "implement combat"; instead "add a
  versioned hostiles table to StatePacket", then "implement guest attack
  request handling", then "review the diff for duplicate execution".
- Give Codex the exact files and constraints; it has the repository and
  `AGENTS.md`, not our conversation.
- Read-only for research and review; workspace-write only for a scoped change
  the Lead already agreed to.
- After every implementation call: Lead reads the diff, runs the build,
  rejects or fixes, and only then commits. Codex does not commit, push, deploy
  or publish.
- Save Codex's answer (`--json` or plain output redirected to a file) under
  `local/` or a temp path, not in the repository.

## Division of labour

| Lead (DSH) | Codex subagent |
|---|---|
| architecture, protocol design, milestone order | focused investigation with file:line evidence |
| runs the game, tests, deployment, git, PRs | isolated implementation of one agreed component |
| decides what is accepted | independent review of a diff (lifetime, threading, desync) |
| owns player-visible behaviour and honesty about what ran | static reasoning over the code |

## Current task order (combat arbitration MVP)

1. Nearby hostiles table in the state packet (host authoritative).
2. Guest attack request (intent, never computed results) with request ids.
3. Host-side validation and execution through `dao::orders::*` so DAO resolves
   the fight; deduplicate request ids.
4. Result sync through the existing hostile state channel.
5. Automated two-client test for the whole chain.
