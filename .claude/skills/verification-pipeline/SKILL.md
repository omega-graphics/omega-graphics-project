---
name: verification-pipeline
description: >
  Runs the Supervised Multi-Phase Verification Architecture. Triggers on
  "verify", "run the pipeline", "check this code", "full verification",
  "security check", "just check the symbols", or "continue verification".
---

You are orchestrating the Verification Pipeline. Follow the procedure in
`VERIFICATION_PIPELINE.md` (Sections 3–7) exactly.

## IMPORTANT: python3

On this system, use `python3` instead of `python` for all CLR commands:
- `python3 .verification/clr_compute.py ...`
- `python3 .verification/clr_calibrate.py ...`

## Session Start

At the beginning of every pipeline session:

```bash
python3 .verification/clr_compute.py --reset
```

## Scope Selection

| User says...                        | Pipeline scope                          |
|-------------------------------------|-----------------------------------------|
| "just check the symbols"            | Phase 2 symbol verification only        |
| "verify" / "run the pipeline"       | Full symbol + analyst pipeline           |
| "security check"                    | Full pipeline + security profile required|
| "full verification"                 | Full pipeline + all profiles             |
| "prototype" / "quick draft"         | No pipeline — generate and done          |

## CLR Gate (CRITICAL)

Before EVERY subagent invocation, run:

```bash
python3 .verification/clr_compute.py --agent {AGENT_NAME} \
    --files {CODE_FILES} --specs {SPEC_FILES} --profile {PROFILE_FILES}
```

Exit codes: 0=PROCEED, 1=PARTITION, 2=ABORT. If non-zero, follow the
`recommendations` in the JSON output before dispatching.

## Phase Execution

Follow the orchestration procedure in VERIFICATION_PIPELINE.md Section 3,
phases 1 through 6, in order. Key rules:

- Each subagent gets a FRESH invocation (separate Agent call).
- Record every dispatch: `python3 .verification/clr_compute.py --record-dispatch {agent}`
- Check supervisor health at phase boundaries: `python3 .verification/clr_compute.py --supervisor`
- Check for κ-transitions before phase changes: `python3 .verification/clr_compute.py --transition --from-phase {N} --to-phase {M}`
- When health=RED or mandatory_spawn=true with YELLOW+ health: checkpoint and suggest fresh session.

## Session Boundary Protocol

When a fresh session is needed, say:

> **Pipeline checkpoint.** Phase {N} is complete. All state is saved
> to `.verification/`. For best results on Phase {N+1}, I recommend
> starting a fresh session. Say: "Continue verification from Phase {N+1}"
> and I will pick up where we left off.

## Resuming

When the user says "continue verification from Phase N":
1. Reset supervisor: `python3 .verification/clr_compute.py --reset`
2. Read MANIFEST.md and all relevant .verification/ state files.
3. Continue from the specified phase.

## Output

After the pipeline completes, present the summary format from
VERIFICATION_PIPELINE.md Section 7.
