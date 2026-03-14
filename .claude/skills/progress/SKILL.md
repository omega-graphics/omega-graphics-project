---
description: Show progress report across all task files
user-invocable: true
---

# /progress — Task Progress Report

The user wants to see the current state of the project.

## Steps

1. Read all `.md` files in `.tasks/` (excluding `README.md`).

2. For each file, extract:
   - Filename
   - **Status** field value
   - Count of `- [x]` (completed tasks)
   - Count of `- [ ]` (remaining tasks)
   - **Depends on** links
   - **Blocks** links
   - Any items marked `**BLOCKED**`

3. Present a summary table:

```
| Task File                  | Status      | Done | Left | Blocked |
|----------------------------|-------------|------|------|---------|
```

4. Show totals: total tasks, completed, remaining, % complete.

5. Show **critical path**: the longest chain of dependent incomplete tasks from start to end.

6. Show **ready to work on**: task files whose status is "Not Started" and whose dependencies are all "Complete".

7. Show **blocked items**: any individual tasks marked `**BLOCKED**` with their reason.

Keep the output concise. No commentary — just the data.
