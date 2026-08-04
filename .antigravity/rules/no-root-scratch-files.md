---
name: no-root-scratch-files
description: Strictly forbids creating temporary scratch scripts or test files in the project workspace.
---

# No Root Scratch Files

1. **Never Create Scratch Files in the Workspace**: Do not create temporary scripts (e.g., `.py` scripts for refactoring) or temporary test files (e.g., `.c` files for testing compiler flags) in the project workspace (e.g., `/home/ubuntu/evhttp`).
2. **Use the Designated Scratch Directory**: Always write temporary/scratch files to the dedicated agent scratch directory: `<appDataDir>/brain/<conversation-id>/scratch/`. You can find the exact path for the current conversation in your system prompt.
3. **Run from Scratch**: When running these temporary scripts, execute them from their location in the scratch directory while pointing them to the workspace if necessary.
4. **Proactive Cleanup**: Even when using the scratch directory, proactively remove one-off files when they are no longer needed for the task at hand.
