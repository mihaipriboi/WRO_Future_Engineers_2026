# Internal Contribution Guidelines

This file outlines the commit and documentation rules to be followed by the team.

---

## Commit Message Format

We use semantic commit messages for clarity and traceability. Every commit must follow this structure:

```
<type>: <short summary>

<optional longer explanation>
```

### Accepted Types
- `feat` – New feature (hardware, software, config)
- `fix` – Bug fix or issue resolution
- `refactor` – Code restructuring without behavior changes
- `docs` – Edits to documentation (README, logs, etc.)
- `test` – Add or modify test cases or simulation scenarios
- `chore` – Maintenance tasks (clean-up, config files)
- `build` – CAD, electrical, mechanical file updates

### Examples
```
feat: integrate IMU calibration script

docs: document wiring changes in chassis v3

fix: correct LIDAR angle offset in obstacle detection
```

Guidelines:
- Write in English
- Summary line should be under 72 characters
- Use imperative tone (e.g., "add", "fix", not "added" or "fixed")
- One commit per logical change

---

## Commit Log Documentation

For significant changes, a corresponding Markdown file should be created in `commit-log/` using this structure:

**File name format:** `YYYY-MM-DD_short-description.md`

**Template:**
```
# [Short Title]

- **Date:** 2025-MM-DD
- **Author:** [Initials or name]
- **Commit hash (optional):** abc123
- **Summary:** What was changed
- **Reason:** Why it was necessary
- **Impact:** Effects on other modules or subsystems
- **Testing status:** one of the following
    - Fully tested
    - Partially tested
    - Not tested
```

This helps maintain a detailed engineering log and supports final documentation requirements.

---

## Structure and Standards

- Keep all diagrams, CAD, and hardware documentation in their assigned folders.
- Write all documentation in clear, technical English.
- Maintain consistent file naming across all assets.
- Avoid merging large, unrelated changes into a single commit.

---

This file is maintained for the exclusive use of the team and reflects internal development standards.
