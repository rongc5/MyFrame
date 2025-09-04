# Contributing

Thanks for your interest in improving MyFrame!

- Start with the repository guide: see `AGENTS.md` (Repository Guidelines).
- Before opening a PR, run local tests:
  - Unit/examples: `cd test && make && ./test_server_simple`
  - Architecture cases: `cd tests && bash run_all.sh`
- Ensure CI passes (`.github/workflows/test.yml` runs `tests/run_all.sh`).
- Follow Conventional Commits (e.g., `feat:`, `fix:`, `test:`, `build:`, `refactor:`).
- Include a clear description, test plan (commands + expected output), and link related issues.

Prerequisites: g++/make, optional CMake, OpenSSL dev (`libssl-dev`), pthreads.

For project structure, coding style, and security notes, see `AGENTS.md`.

