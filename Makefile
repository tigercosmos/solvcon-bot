# modmesh-bot — developer workflow
#
# Local entry point. CI (.github/workflows/ci.yml) shells out to the
# same targets, so what works here works in CI and vice versa.
#
#   make                  build + unit tests (default; no network)
#   make build            configure + compile only
#   make test             run unit tests only (assumes build done)
#   make clean            rm -rf build/
#
#   make e2e              run EVERY e2e scenario (reads .env)
#   make e2e-<name>       run one scenario:
#                           ping, truncated, failure, idempotency,
#                           agent-fake, preflight, auto
#   make all              build + unit + e2e
#
# .env at the repo root drives the e2e targets. See .env.example and
# scripts/e2e_lib.sh for the variables. Pass-throughs:
#   REVIEWER_KIND=claude     swap default mock for a real reviewer
#   REVIEWER_EFFORT=high     bias claude/codex reasoning depth
#   E2E_KEEP_ARTIFACTS=1     keep PR comments/approvals after a run
#   E2E_TIMEOUT_SEC=N        wait longer for AI reviewers

BUILD_DIR ?= build
CMAKE      ?= cmake
CTEST      ?= ctest

E2E_SCENARIOS := ping truncated failure idempotency agent-fake preflight auto
E2E_TARGETS   := $(addprefix e2e-,$(E2E_SCENARIOS))

.PHONY: default build test check clean help codexmon \
        e2e $(E2E_TARGETS) all

# e2e scenarios must NOT run in parallel: each one posts to and polls
# from the same PR, restarts the bot, and mutates state files / lock
# files. .NOTPARALLEL serializes the whole make invocation, which is
# fine — none of our targets benefit from parallelism beyond what
# cmake --build does on its own.
.NOTPARALLEL:

default: build test

# --- build + unit ----------------------------------------------------------

build:
	./scripts/apply_modmesh_patches.sh
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$${CMAKE_BUILD_TYPE:-Release}
	$(CMAKE) --build $(BUILD_DIR) --parallel

test check:
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

# Install the pinned codexmon release (the wrapper that runs every AI
# reviewer kind). Default dest: ~/.local/bin; override with
#   make codexmon CODEXMON_DEST=/usr/local/bin
codexmon:
	./scripts/install_codexmon.sh $(CODEXMON_DEST)

# --- end-to-end ------------------------------------------------------------

# Each scenario depends on `build` so source changes are picked up
# before the e2e runs. cmake's incremental no-op is ~1s when nothing
# changed, which is dwarfed by the e2e scripts themselves.
e2e-ping:        build ; ./scripts/e2e_ping.sh
e2e-truncated:   build ; ./scripts/e2e_truncated_diff.sh
e2e-failure:     build ; ./scripts/e2e_reviewer_failure.sh
e2e-idempotency: build ; ./scripts/e2e_idempotency.sh
e2e-agent-fake:  build ; ./scripts/e2e_agent_fake.sh
e2e-preflight:   build ; ./scripts/e2e_preflight.sh
e2e-auto:        build ; ./scripts/e2e_auto.sh

# Run every e2e scenario, sequentially (.NOTPARALLEL above guarantees
# this even under `make -j`). A failure halts the run; pass `-k` to
# keep going.
e2e:
	@for s in $(E2E_SCENARIOS); do \
	    echo ">>> e2e-$$s"; \
	    $(MAKE) "e2e-$$s" || exit $$?; \
	done

all: default e2e

# --- help ------------------------------------------------------------------

help:
	@printf 'modmesh-bot make targets:\n'
	@printf '  %-18s %s\n' 'make'             'build + unit tests (default; no network)'
	@printf '  %-18s %s\n' 'make build'       'configure + compile'
	@printf '  %-18s %s\n' 'make test'        'run unit tests'
	@printf '  %-18s %s\n' 'make clean'       'rm -rf $(BUILD_DIR)'
	@printf '  %-18s %s\n' 'make codexmon'    'install the pinned codexmon release binary'
	@printf '  %-18s %s\n' 'make e2e'         'run every e2e scenario (reads .env)'
	@for s in $(E2E_SCENARIOS); do \
	    printf '  %-18s %s\n' "make e2e-$$s" "single e2e scenario: $$s"; \
	done
	@printf '  %-18s %s\n' 'make all'         'build + unit + e2e'
