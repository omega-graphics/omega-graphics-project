---
description: Archive a task file with a date and reason
user-invocable: true
---

# /task-archive — Archive a Task File

The user wants to move a completed, descoped, or otherwise finished task to the archive.

## Steps

1. **Identify the task.** If the user names one, find it in `.tasks/`. If not, list task files and let the user pick.

2. **Read the task file.** Show the user a brief summary: title, status, completion count.

3. **Get the reason** if the user hasn't already provided one. Common reasons:
   - "Complete" — all work finished
   - "Descoped" — cut from scope
   - "Superseded by {other-task}" — replaced by a different task
   - Any freeform reason the user gives

4. **Add an archive header** to the task file, directly below the status/dependency block:

```markdown
**Archived**: {YYYY-MM-DD}
**Archive reason**: {reason}
```

5. **Move the file** from `.tasks/` to `.tasks/archive/`. Create the `archive/` directory if it doesn't exist.

6. **Update references.** Find any other task files in `.tasks/` that reference this file in their **Depends on** or **Blocks** fields. Update the links to point to `archive/{filename}`.

7. **Confirm** to the user: show the archived file path and the reason.
