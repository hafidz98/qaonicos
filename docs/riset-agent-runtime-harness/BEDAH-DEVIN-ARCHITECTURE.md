# Devin AI (Cognition) — Public Architecture Teardown

**Purpose:** research input for the Qabot/agent-harness docs/plan comparison (vs. Muse's own architecture and Transcend Rails policy-gating).
**Date:** 2026-10-09. **Method:** public web search + fetching primary sources (Cognition blog, devin.ai blog, docs.devin.ai). No leaked internals; where Cognition is silent, this is marked **UNKNOWN**.
**Claim tags:** `[VERIFIED]` = from Cognition's own publications · `[INFERRED]` = third-party analysis or secondary reporting · `[UNKNOWN]` = not publicly documented.

---

## 1. The Agent–Computer Interface (ACI)

### 1.1 Cloud Devin: full VM + native developer tools
Devin runs inside a **sandboxed compute environment (a dedicated VM per session)** equipped with the same tools a human developer uses: **shell, code editor, and browser** [VERIFIED — https://cognition.com/blog/introducing-devin]. Each cloud session gets **its own VM, created for the task and removed when the work finishes** [VERIFIED — https://cognition.com/blog/aws-sca]. Per-session audit-log APIs record user actions for monitoring/compliance tooling [VERIFIED — https://cognition.com/blog/aws-sca].

Capabilities demonstrated at launch (2024): learn unfamiliar technologies from a blog post, build and deploy apps end-to-end, autonomously find/fix bugs, set up LLM fine-tuning from a GitHub link, and resolve real GitHub issues unassisted [VERIFIED — https://cognition.com/blog/introducing-devin].

The cloud product has since grown: **child/parallel sessions, Slack/GitHub/Linear triggers, DeepWiki** (auto-generated codebase wikis with architecture diagrams and dependency maps), **AskDevin** (Q&A over code with citations), **Knowledge** (org-level shared context), **Secrets manager**, **Machine Snapshots** (VM save states) [INFERRED — sdtimes.com (2024, quoting Cognition); TechCrunch (2025)].

### 1.2 Devin CLI (2026): the public, inspectable harness
Devin CLI is a from-scratch local terminal agent (custom Rust rendering) that exposes the harness mechanics the cloud product only gestures at [INFERRED — steveash research notes on the Cognition announcement]. Its documented surface:

- **Tool set:** `read`, `edit`, `grep`, `glob`, `exec` (shell), web fetch, MCP tools (`mcp__server__tool` naming), background command support (long commands auto-move to background) [VERIFIED — https://docs.devin.ai/cli/reference/permissions; amirkiarafiei skill mirror on the background behavior].
- **Agent modes:** `/normal`, `/plan` (read-only planning), `/ask` (question mode) — separate from permission modes [VERIFIED — https://docs.devin.ai/cli/subagents; freej2 skill mirror].
- **`/handoff`:** transfers the current session — repo, branch, conversation context, uncommitted diff — to a cloud Devin VM [INFERRED — steveash notes on the official announcement].
- **Plugins:** contribute rules, hooks, MCP servers, and custom subagents (`agents/<name>/AGENT.md` profiles surfaced as `<plugin>:<agent>`), Claude-compatible manifests accepted [INFERRED — zalegzl devin-agent changelog mirror of Devin CLI release notes].
- **`.devin/config.json`** holds permissions, hooks, subagent settings; project, user-global, and local-override layers [VERIFIED — https://docs.devin.ai/cli/reference/permissions].

### 1.3 Subagents (first-class delegation primitive)
The CLI documents a full subagent system [VERIFIED — https://docs.devin.ai/cli/subagents]:
- A subagent **shares tools and codebase context with the parent but operates in its own conversation chain** — it does not inherit the parent's conversation history.
- Two execution modes: **Foreground** (parent pauses, approval prompts name the requesting subagent) and **Background** (parent continues; background subagents **inherit already-granted session permissions and auto-deny anything not pre-approved** — they cannot prompt).
- Built-in profiles: `subagent_explore` (read-only: explore + web search; runs on the cheaper router-selected default subagent model) and `subagent_general` (full tools; runs on the parent's model). Custom profiles are markdown files (`agents/<name>.md` or `agents/<name>/AGENT.md`) with frontmatter: `name`, `description`, `model` (pin a cheaper model for write-capable workers), `allowed-tools`, `max-nesting`.
- **Nesting is off by default** — only the root agent can spawn; `max-nesting` opt-in per custom profile. Subagents never get `ask_user_question`.
- **Interruption parks subagents with state intact** (they resume on the next message); cancelled/failed/completed subagents can be resumed with a new prompt (always in foreground).
- Each subagent is its own cost session; cost scales with fan-out, so "use subagents deliberately."
- Enterprise admins can pin the default subagent model or disable subagents entirely.
- Cognition claims subagents "both improve overall coding performance and reduce cost" (no published numbers/methodology — vendor claim, unverified).

### 1.4 Model-agnostic by design
The harness is not tied to one model: Fusion pairs **Claude Fable 5.1** or **GPT-6 Astra** as lead with **SWE-2** as sidekick; CLI supports Opus/GPT-family models via picker, and `/model adaptive` routes simple tasks to cheap models automatically [VERIFIED — https://cognition.com/blog/local-fusion; hummbl-io skill mirror]. Cognition's own **SWE-2** (launched Sept 2026) is a coding model **post-trained from Moonshot AI's Kimi K3 with RL for agentic coding** (reported secondary, citing Cognition's launch article) — no local weights or standalone API published [INFERRED — aimadetools.com].

**Steal-worthy (interface):** ACI = shell + editor + browser in a throwaway VM; model-agnostic harness; background execution of long commands; foreground/background subagent split with permission inheritance rules.

---

## 2. Long-Horizon Execution: Planning, Recovery, Coherence

### 2.1 Plan-first execution loop
- Devin's defining loop since launch: the agent **creates a detailed step-by-step plan first, then executes it with developer tools**, fixing issues and testing along the way, reporting progress in real time; the user can interject corrections mid-task [VERIFIED — https://cognition.com/blog/introducing-devin; siliconangle.com interview with Scott Wu, 2024].
- Cognition frames its core advance as "long-term reasoning and planning": Devin can "plan and execute complex engineering tasks requiring thousands of decisions," **recall relevant context at every step, learn over time, and fix mistakes** [VERIFIED — https://cognition.com/blog/introducing-devin].
- Internal Fusion telemetry on FrontierCode shows **where agent turns actually go: Plan 32%, Setup 34%, Implementation 5%, Debug 17%, Validate 9%** (+ closeout) — i.e., the bulk of a long run is planning, environment setup, and debugging, not writing code [VERIFIED — https://cognition.com/blog/local-fusion]. *Design implication for a small harness: optimize for plan/setup/debug loops, not code generation.*
- The `/plan` mode (read-only) exists as a first-class agent mode, and **Playbooks** give Devin step-by-step templates with **success criteria and guardrails** for recurring task types, minimizing back-and-forth [INFERRED — sdtimes.com 2024, quoting Cognition's launch post].
- A third-party teardown describes autonomous planning with dynamic re-planning over a task DAG [INFERRED — medium.com/@nitinmatani22]. The exact internal plan representation and replanner are **UNKNOWN** — Cognition never published them.

### 2.2 Failure recovery
- Recovery is built on the **act → observe → fix** loop: Devin runs code/tests in its VM, reads failures, and iterates [VERIFIED behaviorally from launch demos — https://cognition.com/blog/introducing-devin].
- **Machine Snapshots** let sessions resume from a saved VM state or start new sessions from a pre-installed environment [INFERRED — sdtimes.com 2024, quoting Cognition].
- In the CLI, long-running commands are **moved to the background automatically** (no lost work on timeout), and **interrupting the parent parks subagents instead of killing them** [VERIFIED — https://docs.devin.ai/cli/subagents].
- The Fusion lead **reviews every sidekick result, identifies problems, and can take control back when the sidekick is out of its depth** — recovery-by-escalation rather than retry-blindly [VERIFIED — https://cognition.com/blog/local-fusion].

### 2.3 Coherence over long runs
- **Session state persists**: sessions "sleep" rather than terminate; full VM state is snapshotted for reuse [INFERRED — signalnine/thunderdome survey of Devin].
- **Context compaction** exists as a lifecycle event (`PostCompaction` hook fires after it) [VERIFIED — https://docs.devin.ai/cli/extensibility/hooks].
- **Repo indexing / DeepWiki** gives persistent codebase understanding independent of the context window [INFERRED — signalnine/thunderdome survey; TechCrunch 2025].
- **Memory & Dreaming (Oct 5, 2026)** — Cognition's newest published mechanism [VERIFIED — https://devin.ai/blog/memory-and-dreaming]:
  - *Memory*: per-user **Memory Drive** — a persistent **Git repo of markdown notes**; a short `MEMORY.md` (preferences + index) is injected at session start; the agent searches/reads deeper notes with its normal tools. Memories are **lessons with links back to the source session**, not session summaries. Captured proactively during work, no human in the loop.
  - *Concurrency*: each session works on **its own Git checkout**; after edits it commits, merges, and pushes. **Optimistic concurrency: a revision check rejects stale writes** (retry against the newer version); conflicting edits are **surfaced for resolution, never silently overwritten**.
  - *Dreaming*: a **daily background session** that revisits memory with global context — dedupes/consolidates overlapping notes, removes transient details, mines lessons missed during the work, and deletes stale records no session used.
  - *Memory vs skills*: "Skills capture a repeatable workflow; memory captures what Devin learns while working with you." Skills are deliberately packaged; memory is accumulated and curated by dreaming.
  - Cognition **open-sourced the memory standard** for other agents to adopt: https://cognition.ai/agent-memory-repo (format: one-line bullet entries with `[source: <session-url>; added: YYYY-MM-DD]` metadata, `[[path]]` cross-links, composable multi-repo loading, and a documented **swarm pattern** where parallel agents share one memory repo as a message board — `findings.md`/`questions.md`/per-agent notes — with git flagging conflicting edits).
- **Knowledge** (org-level): tips, instructions, and org context auto-recalled as relevant [INFERRED — signalnine/thunderdome survey].
- 2025 retrospective notes Devin is now "senior-level at codebase understanding but junior at execution," **4× faster at problem solving, 2× more resource-efficient, 67% PR merge rate** (vs 34% in 2024), and that **clear upfront requirements + verifiable outcomes** are where it excels; mid-task requirement changes remain a weakness [VERIFIED — https://cognition.com/blog/devin-annual-performance-review-2025].

**Steal-worthy (long-horizon):** plan-first with an explicit read-only plan mode; playbooks with success criteria; Git-backed memory with optimistic concurrency; a scheduled "dreaming" consolidation pass; background subagents that park (not die) on interrupt; the Plan/Setup/Implementation/Debug/Validate turn-budget mental model.

---

## 3. Known Architectural Details (from Cognition publications)

### 3.1 Results & benchmarks
| Item | Detail | Source |
|---|---|---|
| SWE-bench (2024 launch) | **13.86%** resolved end-to-end, unassisted (25% subset), vs prior SOTA 1.96% (assisted models capped at 4.80% even with exact files given) | [VERIFIED — https://cognition.com/blog/introducing-devin] |
| SWE-2 + Fusion | SWE-2 scores 50.0% on FrontierCode 1.1 main (within 1 pt of Fable 5.1) at ~64% lower cost; Fusion (Fable 5.1 + SWE-2) hits **68.8 on FrontierCode 1.1 Extended at $0.60/task** | [VERIFIED vendor figures — https://cognition.com/blog/local-fusion; startupfortune.com] |
| Third-party eval | Artificial Analysis + Vals AI co-evaluated Fusion: e.g., DeepSWE 1.1 Fable 64.3@$14.63 → Fusion 63.1@$7.88 (−46%); Vals Code Migration 54.6 → 57.3 while −41% cost | [VERIFIED — https://cognition.com/blog/local-fusion] |
| Security Swarm (Jul 2026) | Agentic map-reduce: parallel agents reason across files; **each finding reproduced in an isolated sandbox to confirm exploitability at runtime**; 36/50 real CVEs found (best of tested scanners), 30% lower cost per finding; patch + PR per confirmed vuln | [Cognition PR via prnewswire.com — https://www.prnewswire.com/news-releases/cognition-launches-devin-security-swarm-to-tackle-the-vulnerability-bla-302814800.html] |
| Production self-use | Wu: Devin writes **~90% of Cognition's production commits** (2026; was 13% a year earlier) | [INFERRED — a16z "Investing in Cognition" via explainx.ai; beritaja/TechCrunch interview] |

### 3.2 Fusion: the lead–sidekick harness (Sept 2026) — the most detailed public architecture Cognition has ever published
All points [VERIFIED — https://cognition.com/blog/local-fusion] unless noted:
- **Structure:** two parallel agents, each with **its own persistent context and tools**. The **lead** (frontier model) owns the plan, ambiguity interpretation, and review; the **sidekick** (cheaper model) gets a **bounded brief with constraints and success criteria**, then explores code, implements, runs tests, and reports back.
- **Communication design:** lead and sidekick exchange **only briefs, results, and feedback — never full conversations**. Each agent builds its own persistent context, "taking full advantage of prompt caching." *This is explicitly the anti-router argument: initial-prompt routing can't judge difficulty, and mid-task model swaps break prompt caches.*
- **Control invariant:** "frontier intelligence is always in charge" — the lead reviews everything and **takes control back when the sidekick is out of its depth**.
- **Per-pair harness tuning** (Cognition tunes the harness per model pair, not just the model choice):
  - *Brief detail:* weaker sidekick → lead writes more prescriptive briefs (more lead tokens upfront, fewer review rounds later); stronger sidekick → leave implementation details to it.
  - *Pushback:* stronger sidekicks are encouraged to challenge the lead's plan (catches lead mistakes); weaker sidekicks allowed to be opinionated hurts performance.
  - *Exploration boundary:* planning-shaping exploration is **not** delegated to a weak sidekick (it shapes the plan); stronger sidekicks are encouraged to support initial exploration.
- **Cost findings:** evaluate **price per task, not price per token** — a nominally 2×-priced lead (Fable 5 over Opus 4.8) produced **9% cheaper sessions** because it delegated earlier, wrote better briefs, and didn't micromanage/redo the sidekick's work. Same for the sidekick: SWE-2 at 3.75× the token price of GPT-5.6 Luna was −2% cheaper overall because fewer attempts + fewer lead review rounds.
- **Turn anatomy** (see §2.1): planning + setup dominate; implementation is ~5% of turns.

### 3.3 Enterprise deployment
- Single-tenant AWS deployment, customer-chosen region, data stays in a **dedicated VPC**; audit-log APIs feed existing monitoring/compliance tools [VERIFIED — https://cognition.com/blog/aws-sca].
- Compliance posture claimed: **SOC 2 Type II, CAIQ questionnaire, self-hosted options** [INFERRED — slayzone summary citing docs.devin.ai/enterprise/security].
- Strategic partnerships for distribution: AWS (Sept 2026), plus Infosys/Cognizant system-integrator deals [INFERRED — explainx.ai; prnewswire].

---

## 4. Safety / Guardrails & Evaluation Methodology

### 4.1 The five-mode permission ladder (CLI — public and precise)
[VERIFIED — https://docs.devin.ai/cli/reference/permissions]
| Mode | Behavior |
|---|---|
| `normal` (default) | Read-only auto; writes/shell/fetch **prompt** |
| `accept-edits` | Workspace file edits auto; shell/fetch prompt |
| `smart` | Edits auto; **a fast model judges** every shell/fetch/MCP action — auto-runs only if *clearly* safe, else prompts. **Never auto-approves**: package installs, mutating `git`, `rm`/`sudo`, `kubectl delete`, cloud-CLI destruction, dotenv/key/Git-config/agent-config access |
| `dangerous`/`bypass`/`yolo` | All tool calls auto-approved |
| `autonomous` (**requires `--sandbox`**) | Shell/fetch auto-approve *inside* the OS sandbox (macOS Seatbelt; Linux bubblewrap+socat); `edit`/`write` still prompt (run outside the sandbox); `Write(...)` scope grants dynamically expand the sandbox |
| Agent modes (orthogonal) | `/plan` read-only, `/ask` Q&A |

### 4.2 Rule resolution (deterministic, not prompt-based)
- Config matchers: `Read(glob)`, `Write(glob)`, `Exec(prefix)`, `Fetch(url/domain)`, tool names, `mcp__server__tool` patterns. Resolution order: **deny > ask > allow > default-prompt**; specificity carves exceptions at the same level; precedence: **org/team settings > session grants > project-local > project config > user config** [VERIFIED — https://docs.devin.ai/cli/reference/permissions].
- A third-party mirror of the docs distills the principle: *"Enforcement belongs in permissions or hooks, not prose. 'Never run X' in AGENTS.md is a request; `deny: ["Exec(X)"]` is a guarantee."* [INFERRED — freej2 skill mirror of Devin CLI config reference]
- Interactive prompts offer allow-once / session / project / project-local / global persistence, plus **inline command editing** before approval [VERIFIED — https://docs.devin.ai/cli/reference/permissions].

### 4.3 Hooks: policy as code on the lifecycle
Eight lifecycle events, verified in the official docs [VERIFIED — https://docs.devin.ai/cli/extensibility/hooks]:
`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PermissionRequest`, `Stop`, `PostCompaction`, `SessionEnd`.
- Hooks are shell commands or LLM prompts receiving **JSON on stdin** (with `session_id` + per-turn `prompt_id` correlation ids, `DEVIN_PROJECT_DIR` env).
- Outputs: `decision: approve|block` (+`reason`), `additionalContext` injection, or **`updatedInput`** (transparently rewrite tool args before execution). Exit code **2 blocks**; other non-zero = logged, non-blocking.
- Locations: `.devin/hooks.v1.json`, `.devin/config.json`, plus Claude-Code-compatible `.claude/` paths; `/hooks` verifies what's loaded.

### 4.4 Sandboxing & isolation
- Cloud: per-task dedicated VM, destroyed after [VERIFIED — https://cognition.com/blog/aws-sca]; CLI `--sandbox`: OS-level isolation for exec processes (Seatbelt on macOS; bubblewrap+socat on Linux), writable roots, deny rules, domain filtering [INFERRED — mia-cx skill mirror of docs.devin.ai/cli/sandbox].
- Security Swarm reproduces every suspected vulnerability **in an isolated sandbox to confirm exploitability at runtime** before writing the patch [Cognition PR — prnewswire link above].

### 4.5 Evaluation methodology
- **SWE-bench** end-to-end (unassisted) as the founding eval [VERIFIED — introducing-devin].
- **Vendor + third-party co-evaluation** for Fusion (Artificial Analysis, Vals AI) with cost-per-task as the headline metric [VERIFIED — local-fusion]. *Caveat: vendor-designed benchmarks; independent replication pending.*
- **Open benchmarked security eval**: 50 real-world vulns tied to published GitHub Security Advisories, 14 languages [Cognition PR — prnewswire link above].
- **Internal production as eval**: the 90%-of-commits dogfooding loop [INFERRED — a16z/beritaja].
- Scott Wu's stated principle: "**it should always be up to the human what to do**" — Devin as teammate, humans stay in control; capability self-described as "somewhere between a junior and a mid-level engineer" depending on task [INFERRED — imp.news; beritaja.com summarizing TechCrunch Disrupt interview].

### 4.6 What Cognition does NOT publish (UNKNOWN)
- The cloud agent's system prompt, planner representation, or replanning algorithm.
- Internal red-teaming methodology, jailbreak-resistance evals, or alignment training details for Devin/SWE-2.
- Whether the cloud agent has tool-call approval policies beyond the CLI-documented ladder (the CLI docs are the public window).
- Fine-grained telemetry privacy handling inside customer VPCs.

---

## 5. Steal-Worthy Patterns for a Lightweight Agent Harness (Qabot-scale)

These are concrete, lab-free patterns extracted from the verified material above, ordered by effort-to-value for a small harness.

### 5.1 Fusion-style lead–sidekick with brief-only exchange
- **Pattern:** two agents, two persistent contexts; the only cross-traffic is **bounded briefs (task + constraints + success criteria) → results → review feedback**. Never shuttle full histories between models.
- **Why it scales down:** prompt-cache efficiency is a real cost lever even for small setups; brief-only exchange also *forces* task decomposition discipline.
- **Three tuning knobs** (from Cognition's per-pair tuning): brief prescriptiveness, sidekick pushback allowance, exploration-delegation boundary. Hard-code these as config per model pair, not as prompt prose.
- **Qabot mapping:** cloud LLM = lead (plan, ambiguity, review); a cheap local/cloud model = sidekick for bounded mechanical work (exploration, test runs). On a 64MB device, sidekick work could even be pure tooling/scripts rather than a second model.

### 5.2 Git-backed memory with optimistic concurrency + scheduled dreaming
- **Pattern:** memory as a **Git repo of markdown files**; `MEMORY.md` index injected at session start; per-session checkouts; commit+merge+push; **revision check rejects stale writes**; conflicts surfaced, never silently overwritten. A **daily background pass** dedupes, de-transients, mines missed lessons, and deletes unused stale entries.
- **Format (open standard, adoptable verbatim):** one-line bullet entries, `[source: <session>; added: YYYY-MM-DD]` metadata, `[[path]]` cross-links, MEMORY.md kept short + index.
- **Qabot mapping:** this is nearly identical to our existing `MEMORY.md` + daily notes habit — the missing pieces are the **stale-write revision check** and the **scheduled dreaming pass** (we already have cron infrastructure; a `kg-analyst`-style daily memory curator is a natural fit). Note: Muse's current memory has no conflict handling for parallel sessions — Devin's answer is git's.

### 5.3 The five-mode permission ladder + deterministic rule resolution
- **Pattern:** `normal → accept-edits → smart → bypass → autonomous(sandboxed)`; read-only always auto; resolution order **deny > ask > allow > prompt**; org > session > project > user precedence; persistent grant scopes (once/session/project/global).
- **The key sentence:** *"Enforcement belongs in permissions or hooks, not prose"* — AGENTS.md-style instructions are requests; `deny: ["Exec(X)"]` is a guarantee.
- **Qabot mapping:** Q1 already has a pre-execution policy gate; Devin adds the graded ladder (not binary allow/block) and machine-checkable matchers (`Read/Write/Exec/Fetch` scopes). The **"smart" mode** — a fast model judging routine actions, with a hard-coded never-auto list (installs, mutating git, rm/sudo, cloud CLIs, secrets) — is directly analogous to our Jev/System-1 decision-model idea, and Devin ships it as the default middle ground.

### 5.4 Lifecycle hooks with block / rewrite / inject
- **Pattern:** 8 events (`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PermissionRequest`, `Stop`, `PostCompaction`, `SessionEnd`); hooks get JSON on stdin with correlation ids; outputs can **block (exit 2), inject context, or transparently rewrite tool arguments** (`updatedInput`) before execution.
- **Qabot mapping:** this is a superset of our guardrail concept. `PreToolUse` input-rewriting is the novel bit — a policy layer that *repairs* rather than only *vetoes*. `PostCompaction` as a hook point is worth copying: re-inject invariants after every compaction.

### 5.5 Subagent permission inheritance rules
- **Pattern:** foreground subagents prompt (named); **background subagents inherit only pre-approved session permissions and auto-deny the rest** — they can never prompt. Interrupts **park** (not kill) subagents with state intact. Nesting off by default; opt-in `max-nesting`. Subagents never get `ask_user_question`.
- **Qabot mapping:** cheap to implement in any harness and prevents an entire class of stuck/hijack states.

### 5.6 Playbooks + Knowledge (procedure vs. context)
- **Pattern:** **Playbooks** = step-by-step templates with success criteria + guardrails for recurring tasks; **Knowledge** = org context auto-recalled as relevant; **Memory** = lessons learned (personal); **Skills** = deliberately packaged reusable workflows. Four distinct layers with different lifecycles.
- **Qabot mapping:** we conflate these today (AGENTS.md + MEMORY.md + skills). Splitting them by lifecycle (packaged vs. accumulated vs. recalled) is a clarity win.

### 5.7 Evaluate on price-per-task, design for the turn budget
- **Pattern:** benchmark harnesses on **cost per completed task**, not per token — a more expensive lead can be cheaper overall (better briefs, earlier delegation, fewer review rounds). And design for where turns actually go: **Plan 32% / Setup 34% / Implementation 5% / Debug 17% / Validate 9%**.
- **Qabot mapping:** when we benchmark model pairings (e.g., cloud lead + local sidekick), score accepted-task cost, not token price. Invest harness effort in plan/debug/validate loops and environment setup reproducibility (snapshots), not in code-gen prompts.

---

## Summary Table: Steal-Worthy Patterns

| # | Pattern | Devin source | Cost to implement (small harness) | Qabot fit |
|---|---|---|---|---|
| 1 | Lead–sidekick, brief-only exchange (never full histories) | [VERIFIED — cognition.com/blog/local-fusion] | Low: two loops + brief schema | High: cloud lead / cheap sidekick split |
| 2 | Per-pair harness tuning knobs (brief detail, pushback, exploration boundary) | [VERIFIED — local-fusion] | Low: config, not code | Medium: applies once two models are in play |
| 3 | Git-backed memory + optimistic concurrency (stale-write rejection) | [VERIFIED — devin.ai/blog/memory-and-dreaming; cognition.ai/agent-memory-repo] | Low: git ops | High: hardens our MEMORY.md habit |
| 4 | Scheduled "dreaming" consolidation pass (dedupe, de-transient, mine, expire) | [VERIFIED — memory-and-dreaming] | Low: a cron + prompt | High: maps to existing kg-analyst cron pattern |
| 5 | Entry format: one-line bullets + `[source; added]` + `[[links]]` | [VERIFIED — agent-memory-repo, open standard] | Trivial | High: adopt verbatim |
| 6 | 5-mode permission ladder (normal/accept-edits/smart/bypass/autonomous) | [VERIFIED — docs.devin.ai/cli/reference/permissions] | Medium | High: extends Q1 policy gate |
| 7 | "Smart" mode: fast model judges routine actions + hard-coded never-auto list | [VERIFIED — permissions docs] | Medium | High: = our Jev/System-1 idea, validated |
| 8 | Deterministic rule resolution deny > ask > allow > prompt; "enforcement in permissions/hooks, not prose" | [VERIFIED — permissions docs] | Low–Medium | High: core policy-gate principle |
| 9 | Lifecycle hooks: block / inject context / rewrite tool input (`updatedInput`) | [VERIFIED — docs.devin.ai/cli/extensibility/hooks] | Medium | High: superset of guardrail; `PostCompaction` hook is novel |
| 10 | Subagent permission inheritance: background = pre-approved only, never prompts; interrupts park, not kill | [VERIFIED — docs.devin.ai/cli/subagents] | Low | High: prevents stuck states |
| 11 | Playbooks (procedure + success criteria + guardrails) vs Knowledge (recalled context) vs Memory (lessons) vs Skills (packaged workflows) | [INFERRED — sdtimes quoting Cognition; VERIFIED memory/skills split — memory-and-dreaming] | Low: organizational | Medium: clarifies our AGENTS.md/MEMORY.md split |
| 12 | Price-per-task evaluation; turn-budget design (plan+setup ≈ 2/3 of turns) | [VERIFIED — local-fusion] | Low: measurement discipline | Medium: guides where to invest |
| 13 | OS-level sandbox for autonomous mode (Seatbelt/bubblewrap); per-task throwaway VMs in cloud | [VERIFIED cloud VM — cognition.com/blog/aws-sca; INFERRED CLI sandbox impl] | High (cloud) / Medium (bubblewrap) | Medium: QEMU sandbox possible; cloud VMs out of scope |
| 14 | Audit-log APIs + per-session VMs for compliance | [VERIFIED — cognition.com/blog/aws-sca] | Medium | Low (enterprise-only concern) |

## Explicitly UNKNOWN (do not design around these)
- Cloud agent system prompt, planner data structures, and replanning algorithm.
- Devin's internal red-teaming / jailbreak evals; SWE-2 training details beyond "post-trained from Kimi K3 with RL for agentic coding."
- Cloud-side approval policies beyond the CLI ladder; telemetry privacy internals.
- Whether Fusion's lead/sidekick split would help on non-coding tasks (all published data is coding benchmarks).
