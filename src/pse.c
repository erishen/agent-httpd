/* PSE three-role orchestrator — see pse.h. Built on the same primitives as
 * the plain ReAct loop: agent_round() for the one-shot phases (tool_choice
 * none, content captured while deltas still stream to the page) and
 * agent_run_ex() for the Specialist's tool-enabled loop. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include "internal.h"
#include "minijson.h"
#include "chatio.h"
#include "agent.h"
#include "pse.h"

#define PSE_SOUL_MAX (1 << 18)   /* 256KB per SOUL.md cap */

int pse_enabled(void) {
    const char *v = getenv("PSE_ENABLED");
    return v && strcmp(v, "true") == 0;
}

/* ---- souls ----------------------------------------------------------- */

/* Resolve the souls root: $PSE_SOULS_DIR -> <cwd>/souls -> skills-dir/../souls. */
static const char *souls_root(void) {
    static char root[MAX_PATH_SIZE * 2];
    static int tried = 0;
    if (tried) return root[0] ? root : NULL;
    tried = 1;

    const char *env = getenv("PSE_SOULS_DIR");
    if (env && env[0]) {
        set_str(root, sizeof root, env);
    } else {
        char cwd[MAX_PATH_SIZE];
        if (getcwd(cwd, sizeof cwd)) {
            snprintf(root, sizeof root, "%s/souls", cwd);
            FILE *probe = fopen(root, "r");
            if (!probe) {
                const char *hd = getenv("HARNESS_SKILLS_DIR");
                if (hd && hd[0]) {
                    snprintf(root, sizeof root, "%s/../souls", hd);
                    probe = fopen(root, "r");
                }
            }
            if (probe) {
                fclose(probe);
                return root;
            }
            root[0] = '\0';
            return NULL;
        }
        root[0] = '\0';
        return NULL;
    }
    return root;
}

/* Read <root>/<role>/SOUL.md. Returns malloc'd body or NULL. */
static char *pse_read_soul(const char *role) {
    const char *root = souls_root();
    char path[MAX_PATH_SIZE * 2];
    if (!root) return NULL;
    snprintf(path, sizeof path, "%s/%s/SOUL.md", root, role);
    if (!strpbrk(role, "\\/.")) {
        FILE *f = fopen(path, "r");
        if (!f) return NULL;
        sbuf b = {0};
        char chunk[4096];
        size_t r;
        while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
            sb_mem(&b, chunk, r);
            if (b.oom || b.len > PSE_SOUL_MAX) break;
        }
        fclose(f);
        return b.p;
    }
    return NULL;
}

/* ---- helpers --------------------------------------------------------- */

static void append_extra(sbuf *sys, const char *base, const char *extra) {
    sb_str(sys, base);
    if (extra && extra[0]) {
        sb_str(sys, "\n\n");
        sb_str(sys, extra);
    }
}

static const char *PSE_PLANNER_DEFAULT =
    "你是 Planner。请将任务分解为可执行步骤并输出计划。你只输出纯文字计划，绝不执行工具。";

static const char *PSE_SPECIALIST_DEFAULT =
    "你是 Specialist。按照 Planner 的执行计划，使用可用工具完成任务，输出具体结果。";

static const char *PSE_EVALUATOR_DEFAULT =
    "你是 Evaluator。请独立评审执行结果，第一行输出 PASS / PARTIAL / FAIL 之一，并给出反馈。";

static int starts_with_pass(const char *s) {
    if (strncasecmp(s, "PASS", 4) != 0) return 0;
    char c = s[4];
    return c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '.';
}

/* One-shot no-tools round: build [system, user], stream, capture content. */
static int pse_round_once(ChatOut *out, const char *sys, const char *user,
                          sbuf *capture) {
    sbuf msgs = {0};
    sb_chr(&msgs, '[');
    sb_str(&msgs, "{\"role\":\"system\",\"content\":");
    sb_json_str(&msgs, sys);
    sb_str(&msgs, "},{\"role\":\"user\",\"content\":");
    sb_json_str(&msgs, user);
    sb_str(&msgs, "}");

    RoundState rs;
    memset(&rs, 0, sizeof rs);
    const char *finish = NULL;
    int up_err = 0;
    int rc = agent_round(out, &msgs, "", "none", capture, &rs, &finish, &up_err);
    free(msgs.p);
    round_free(&rs);
    if (rc != 0) agent_emit_upstream_error(out, up_err);
    return rc;
}

/* Sanitize role output: strip any stray XML-looking tool tags the model
 * dribbles out (resolve-studio does the same). In-place. */
static void sanitize_role_output(char *s) {
    if (!s) return; /* capture may be empty (model returned nothing) */
    char *dst = s;
    const char *p = s;
    while (*p) {
        if (p[0] == '<' &&
            (strncasecmp(p + 1, "tool_call", 9) == 0 ||
             strncasecmp(p + 1, "parameter", 9) == 0)) {
            const char *end = strstr(p, ">");
            if (end) {
                /* look for a matching close tag */
                const char *close = NULL;
                if (strncasecmp(p + 1, "tool_call", 9) == 0) {
                    close = strstr(end, "</tool_call>");
                } else {
                    close = strstr(end, "</parameter>");
                }
                if (close) {
                    p = close + 1;
                    /* include a space */
                    continue;
                }
            }
        }
        *dst++ = *p++;
    }
    *dst = '\0';
}

void pse_run(ChatOut *out, const ChatRequest *req, const char *system_extra) {
    if (!agent_slot_take()) {
        sse_event(out, "error",
                  "server busy (AGENT_MAX_CONCURRENT reached), retry shortly");
        return;
    }

    char task[AGENT_MAX_MESSAGE + 1];
    set_str(task, sizeof task, req->message);

    char *planner_soul = pse_read_soul("planner");
    char *specialist_soul = pse_read_soul("specialist");
    char *evaluator_soul = pse_read_soul("evaluator");
    if (!planner_soul) planner_soul = strdup(PSE_PLANNER_DEFAULT);
    if (!specialist_soul) specialist_soul = strdup(PSE_SPECIALIST_DEFAULT);
    if (!evaluator_soul) evaluator_soul = strdup(PSE_EVALUATOR_DEFAULT);

    sbuf plan = {0};
    sbuf specialist_result = {0};
    char *feedback = NULL;
    int verdict_pass = 0;

    for (int attempt = 1; attempt <= PSE_MAX_ATTEMPTS && out->ok; attempt++) {
        char phase[64];
        snprintf(phase, sizeof phase, "PSE cycle %d/%d - Planner", attempt,
                 PSE_MAX_ATTEMPTS);
        sse_event(out, "note", phase);
        if (!out->ok) break;

        /* Planner: system = soul (+skills/memory); user = task (+feedback) */
        sbuf planner_sys = {0};
        append_extra(&planner_sys, planner_soul, system_extra);
        sbuf planner_user = {0};
        sb_str(&planner_user, task);
        if (feedback && feedback[0]) {
            sb_str(&planner_user, "\n\n## 上一轮 Evaluator 反馈\n");
            sb_str(&planner_user, feedback);
            sb_str(&planner_user, "\n\n请根据反馈重新规划执行步骤。");
        }
        sb_str(&planner_user,
               "\n\n请输出结构化执行计划：1) 任务分解（子步骤列表）2) 每步所需工具 "
               "3) 验收标准。"
               "【硬性要求】你只负责输出纯文字执行计划，不执行任何工具。直接输出计划正文。");
        plan.len = 0;
        if (plan.p) plan.p[0] = '\0';
        int rc = pse_round_once(out, planner_sys.p, planner_user.p, &plan);
        sse_event(out, "note", "[planner]");
        free(planner_sys.p);
        free(planner_user.p);
        if (rc != 0 || !out->ok) break;
        char *plan_s = plan.p ? plan.p : "";
        sanitize_role_output(plan_s);

        /* Specialist: full ReAct loop, tools on */
        snprintf(phase, sizeof phase, "PSE cycle %d/%d - Specialist (tools)",
                 attempt, PSE_MAX_ATTEMPTS);
        sse_event(out, "note", phase);
        if (!out->ok) break;
        ChatRequest spec = *req;
        spec.message[0] = '\0';
        set_str(spec.message, sizeof spec.message, task);
        spec.n_history = 0;
        sbuf spec_extra = {0};
        if (system_extra && system_extra[0]) {
            sb_str(&spec_extra, system_extra);
            sb_str(&spec_extra, "\n\n");
        }
        sb_str(&spec_extra, "## Planner 执行计划\n");
        sb_str(&spec_extra, plan_s);
        specialist_result.len = 0;
        if (specialist_result.p) specialist_result.p[0] = '\0';
        agent_run_ex(out, &spec, specialist_soul, spec_extra.p, &specialist_result);
        free(spec_extra.p);
        if (!out->ok) break;

        /* Evaluator: system = soul (+skills/memory); user = task+plan+result */
        snprintf(phase, sizeof phase, "PSE cycle %d/%d - Evaluator", attempt,
                 PSE_MAX_ATTEMPTS);
        sse_event(out, "note", phase);
        if (!out->ok) break;
        sbuf evaluator_sys = {0};
        append_extra(&evaluator_sys, evaluator_soul, system_extra);
        sbuf eval_user = {0};
        sb_str(&eval_user, "## 原始任务\n");
        sb_str(&eval_user, task);
        sb_str(&eval_user, "\n\n## 执行计划\n");
        sb_str(&eval_user, plan_s);
        sb_str(&eval_user, "\n\n## Specialist 执行结果\n");
        sb_str(&eval_user, specialist_result.p ? specialist_result.p : "(no output)");
        sb_str(&eval_user,
               "\n\n请独立评审上述执行结果。输出第一行必须是以下之一：\n"
               "- PASS — 完全满足验收标准\n"
               "- PARTIAL — 部分满足，列出未完成项\n"
               "- FAIL — 未满足，说明原因和改进建议");
        sbuf verdict = {0};
        rc = pse_round_once(out, evaluator_sys.p, eval_user.p, &verdict);
        sse_event(out, "note", "[evaluator]");
        free(evaluator_sys.p);
        free(eval_user.p);
        if (rc != 0 || !out->ok) break;
        char *verdict_s = verdict.p ? verdict.p : "";
        sanitize_role_output(verdict_s);

        if (starts_with_pass(verdict_s)) {
            verdict_pass = 1;
            free(verdict.p);
            break;
        }
        /* retry with feedback */
        free(feedback);
        feedback = strdup(verdict.p ? verdict.p : "FAIL");
        free(verdict.p);
        if (!feedback) break;
        snprintf(phase, sizeof phase,
                 "PSE cycle %d/%d - not PASS, retrying with feedback",
                 attempt, PSE_MAX_ATTEMPTS);
        sse_event(out, "note", phase);
    }

    if (!verdict_pass && out->ok) {
        sse_event(out, "note",
                  "PSE exhausted attempts; last Specialist output above is the result.");
    }

    free(planner_soul);
    free(specialist_soul);
    free(evaluator_soul);
    free(plan.p);
    free(specialist_result.p);
    free(feedback);
    agent_slot_give();
}