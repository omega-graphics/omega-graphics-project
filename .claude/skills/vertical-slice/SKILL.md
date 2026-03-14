---
description: Scaffold the thinnest vertical slice for a feature
user-invocable: true
---

# /vertical-slice — Scaffold a Feature Slice

The user wants to build a thin vertical slice — one path from user action to system response — for a given feature. This follows the shepherd methodology: build the smallest meaningful piece, show it, let the developer's reaction guide the rest.

## Before writing any code

1. Read the relevant rules in `.claude/rules/`:
   - `typescript.md` — type strictness, Result types, branded IDs
   - `error-handling.md` — expected vs unexpected failures
   - `file-naming.md` — kebab-case, directory structure, co-located tests
   - `dynamodb-patterns.md` — key schema, access patterns, client usage
   - `cdk-conventions.md` — if infra changes are needed

2. Read existing code in the repo to understand current patterns. State what you observe: "Here's what I think the current architecture is..." Let the developer confirm before building.

3. Read the relevant task file(s) in `.tasks/` to understand scope and dependencies.

## The slice

For the feature the user describes, identify the **single thinnest path** that proves the feature works end-to-end:

- **DynamoDB**: The item schema and one access pattern
- **Service function**: One function that performs the operation, returning a Result type
- **API route**: One Next.js route handler that calls the service and translates the Result to HTTP
- **Frontend**: One page or component that calls the API and renders the response

## What to produce

Present the plan to the developer first — don't just start writing. Show:

1. The files you'll create/modify
2. The data flow (request → route → service → DynamoDB → response)
3. Any decisions that need to be made before you can proceed

Then build only after the developer confirms the direction. If they say "just build it," build it and treat their reaction to the code as the feedback.

## After building

- Update the relevant task file(s) — check off completed items.
- Note any decisions made in the task file's **Decisions** section.
- Flag anything that felt wrong or uncertain: "This works but I'm not sure about X."
