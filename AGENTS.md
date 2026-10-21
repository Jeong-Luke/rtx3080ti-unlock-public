# Agent entry point

This repository contains experimental GPU firmware execution code, not an unattended installer.

1. Read `README.md`, `docs/DISCLAIMER.md` and the complete `docs/AGENT_HANDOFF.md` before proposing application.
2. Treat `docs/AGENT_HANDOFF.md` as the shared procedure for Codex, Claude and other assistants. Its recorded observations do not establish the current user's hardware state.
3. Start with read-only compatibility checks. Preserve the original firmware, VBIOS, keys, boot configuration, user data and logs.
4. Do not remove target guards, enable SS1 full changes, execute GPU firmware, reset the GPU, sign files, enroll keys, deploy to USB/ESP or reboot merely because the repository was opened. Obtain the current user's explicit approval for the exact target and each consequential stage.
5. Never expose credentials, private keys, recovery passwords, device serials or local partition identifiers in public output.
6. Do not automatically repeat an uncertain or failed attempt, disable Secure Boot, reset firmware defaults or flash a VBIOS as a workaround.
7. Distinguish source inspection, offline tests, a fresh binary, physical execution, Windows recovery and performance evidence. Do not infer success from historical logs or a chart.

Use `docs/BUILD.md` for builds and `docs/BENCHMARKS.md` for the measured workload. Platform-specific instructions or tools not actually available to you are not capabilities you can claim to have.
