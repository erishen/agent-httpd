#ifndef SKILLS_H
#define SKILLS_H

#include "httpd.h"

/* Skill index (port of resolve-studio's SkillsService, C edition).
 *
 * A "skill" is a directory containing a SKILL.md whose frontmatter carries
 * name + description; the harness scans all skill roots once at startup in
 * the parent process (an immutable snapshot is then shared by every forked
 * worker) and exposes two things:
 *   - skills_index_text(): "- name: description" lines injected into the
 *     agent system prompt so the model knows which workflows exist;
 *   - skills_read(name): the full SKILL.md body (used by the skill-run
 *     tool so the model can act on a skill without being given the whole
 *     index — the injection is a catalog, not the content).
 *
 * Directory search order (earlier wins on collisions), mirroring the TS
 * service: 1) <cwd>/skills 2) $HARNESS_SKILLS_DIR 3) <cwd>/resolve-skills/skills
 * 4) $SKILLS_EXTRA_DIRS (comma-separated). SKILL.md frontmatter:
 *   ---
 *   name: code-review
 *   description: 审查代码改动并输出结构化报告
 *   ---
 *   # steps ...
 */

#define SKILL_NAME_MAX 64
#define SKILL_DESC_MAX 160
#define SKILL_PATH_MAX MAX_PATH_SIZE

typedef struct {
    char name[SKILL_NAME_MAX + 1];
    char desc[SKILL_DESC_MAX + 1];
    char path[SKILL_PATH_MAX + 1]; /* full path to this SKILL.md */
} SkillInfo;

/* Scan + index every skill root. Call once in the PARENT before any worker
 * or connection child is forked. Returns the number of skills indexed
 * (0 when nothing found — the feature simply stays off). */
int skills_init(void);

int skills_count(void);
const SkillInfo *skills_get(int i);

/* "- name: description" lines for the system prompt (static, cached). */
const char *skills_index_text(void);

/* Full SKILL.md body for a skill name; malloc'd (caller free()), NULL when
 * the name is unknown or the file unreadable. Fails closed (no traversal:
 * name is matched against the indexed table, never used as a path). */
char *skills_read(const char *name);

/* Profile allow-list predicate (HARNESS_SKILLS_ALLOW="a,b"; unset/empty =
 * allow everything). Exposed so the router sync can drop disallowed skills
 * BEFORE writing them to disk: with a writable skills/ mount the shared
 * corpus (code-review / hot-news-post / post-comment / rust-review) would
 * otherwise land in the container even though the index filters it later.
 * Keeping them off disk entirely means a cleared allow-list cannot silently
 * re-expose write-capable skills. */
int skills_allowed(const char *name);

#endif /* SKILLS_H */