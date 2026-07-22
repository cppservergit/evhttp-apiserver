---
name: git-workflow
description: Constraints on autonomous git operations.
---

# Git Workflow Constraints
1. **Never autonomously execute `git commit` or `git push`.**
2. You may stage files (e.g. `git add`) or run git status/diff, but you must strictly wait for explicit user permission or a direct command before committing or pushing changes.
3. When you have completed a task, notify the user that the code is ready and wait for their instructions to commit.
