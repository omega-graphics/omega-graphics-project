---
description: Create a new task file following project conventions
user-invocable: true
---

# /task-create — Create a New Task File

The user wants to create a new task file. They will provide a description of the feature or work item.

## Steps

1. Determine the correct filename using the `{layer}-{domain}-{feature}.md` convention:
   - **layer**: `infra`, `backend`, `frontend`, `devops`, `project`
   - **domain**: the system area (e.g. `cdk`, `dynamodb`, `api`, `pages`, `auth`, `ci`)
   - **feature**: the specific thing

2. Ask the user to confirm the filename if it's ambiguous.

3. Read existing task files in `.tasks/` to identify dependency relationships.

4. Create the file in `.tasks/` using this exact template:

```markdown
# {Title}

> {One-line description of what "done" looks like.}

**Status**: Not Started
**Depends on**: {links to dependency files, or "nothing"}
**Blocks**: {links to files this blocks, or "nothing"}

## Tasks

- [ ] {Break the work into concrete, checkable items}
  - [ ] {Subtasks where useful}

## Decisions

- (none yet)

## Notes

- {Anything useful for the implementor. Omit section if nothing to say.}
```

5. Update any existing task files that should list this new file in their **Blocks** field.

6. Show the user the created file and the dependency chain.
