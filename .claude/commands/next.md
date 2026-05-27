---
description: Show pptctl roadmap status and recommend the next issue to work on
---

You are orienting a work session on **pptctl**, whose work is tracked in GitHub milestones (`Phase 0` … `Phase 8`) and Project #5 in the repo `valentin-best/pptctl`.

Run exactly this one command to get the full picture:

```
gh issue list --repo valentin-best/pptctl --state all --limit 100 --json number,title,state,milestone,labels
```

Then, from that JSON:

1. **Group issues by milestone** and compute open/closed counts per phase. Phase order is the `Phase N` number in the milestone title.
2. **Current phase** = the lowest-numbered phase that still has any open issue.
3. **Recommend ONE next issue** to work on, honoring these dependencies (don't recommend blocked work):
   - Issue "resolve `[Content_Types].xml` + relationships" (relationship graph) must land **before** any `extract` work that resolves pictures/notes — it's the foundation.
   - Infra issues (Windows CI, `.clang-tidy`) are low-risk warm-ups that can be done anytime.
   - Within a phase, prefer foundational/parsing issues before output-format and golden-test issues.
4. If `$ARGUMENTS` names or numbers an issue, focus on that one instead: run `gh issue view <n> --repo valentin-best/pptctl` and walk me through how to approach it.

**Report format** (keep it tight, ~12 lines):
- One line: current phase + progress (e.g. `Phase 1 — info: 2 open / 0 closed`).
- The open issues in the current phase, as `#N title`.
- **Next:** the single issue you recommend + one sentence on why.
- End by asking if I want to start it. **Do not start coding unless I say go.**
