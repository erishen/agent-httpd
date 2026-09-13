#ifndef PSE_H
#define PSE_H

#include "agent.h"

/* PSE (Planner-Specialist-Evaluator) three-role orchestrator — port of
 * resolve-studio's runPseOrchestrator, C edition.
 *
 *   Planner   one LLM call, no tools  -> structured execution plan
 *   Specialist full ReAct tool loop   -> executes the plan (tools enabled)
 *   Evaluator one LLM call, no tools  -> PASS / PARTIAL / FAIL + feedback
 *
 * Any non-PASS verdict retries the cycle with the Evaluator's feedback
 * injected into the next Planner call (up to PSE_MAX_ATTEMPTS). The whole
 * run holds a single agent concurrency slot (the phases beneath it are the
 * bare primitives). Role prompts come from $PSE_SOULS_DIR/<role>/SOUL.md
 * (fallback: <cwd>/souls/<role>/SOUL.md, or $HARNESS_SKILLS_DIR/../souls
 * mirroring resolve-studio); role files are read on demand and not cached
 * so a drop-in souls dir works without a restart. */

#define PSE_MAX_ATTEMPTS 3

/* Whether PSE mode is active ($PSE_ENABLED=true). */
int pse_enabled(void);

/* Run one PSE orchestration. req->message is the task; system_extra (skills
 * index + session memory) is appended to every role prompt. Streams phase
 * markers + live deltas via out. Returns 0; stream errors already flipped
 * out->ok. */
void pse_run(ChatOut *out, const ChatRequest *req, const char *system_extra);

#endif /* PSE_H */