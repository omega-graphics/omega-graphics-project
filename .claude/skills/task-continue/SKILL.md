---
description: Continue working on an existing task based on what remains incomplete
user-invocable: true
---

# /task-continue — Continue an Existing Task

The user wants to pick up work on a task. They may name a specific task file or ask what's ready to work on.

## Steps

1. **Identify the task.** If the user names one, read that file from `.tasks/`. If not, read all `.md` files in `.tasks/` (excluding `README.md` and the `archive/` directory) and show tasks whose status is "Not Started" or "In Progress" and whose dependencies are all "Complete". Let the user pick.

2. **Read the task file.** Parse:
   - All `- [ ]` items (remaining work)
   - All `- [x]` items (already done — don't redo these)
   - The **Depends on** field — verify dependencies are actually complete
   - The **Decisions** section — respect prior decisions

3. **Read agent memory.** Check `.claude/agent-memory/` and `.claude/agent-memory-local/` for any notes relevant to this task or its domain. Prior context from earlier sessions matters — don't rediscover what's already known.

4. **Read the current code state.** Before doing anything, look at the files that this task touches or will touch. Understand what already exists. State your read of the current state briefly: "Here's where I think things stand..."

5. **Identify the next incomplete item.** Work through `- [ ]` items top to bottom. The first unchecked item (that isn't blocked) is the next thing to do.

6. **Set status to In Progress** if it's currently "Not Started".

7. **Do the work.** Follow the project's rules (`.claude/rules/`). Build in thin slices. After each meaningful piece, check off the completed `- [ ]` item in the task file.

8. **Record decisions.** If you make architectural choices during implementation, add them to the **Decisions** section of the task file.

9. **Update status when done.** If all `- [ ]` items are checked, set **Status** to "Complete". If work remains, leave it "In Progress" and tell the user what's left.

10. **Update blockers.** If this task completing unblocks another task, note it.
