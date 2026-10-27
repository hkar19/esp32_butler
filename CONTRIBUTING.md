# Contributing

Thanks for helping! The short version: **fork, branch, open a pull request to `main`.**

## Steps

1. Fork this repo and create a branch for your change.
2. Make your change. Keep it focused: one feature or fix per pull request.
3. Check that the firmware still builds:
   ```sh
   cd firmware && ./bt.sh test-build
   ```
4. Run the tests: every executable `tests/test_*` file (see below).
5. Open a pull request to `main`. Say what it changes and why, and how you tested it: which board, if you tried it on
   hardware.

GitHub Actions runs the build check and the tests on every pull request. It must be green before a merge.

## Tests

**New features need a test.** Bug fixes are welcome to bring one too, so the bug can't come back.

A test is an executable file in `tests/` named `test_<something>` (a shell script, or Python with a `#!/usr/bin/env python3`
line). It must exit 0 on success and non-zero on failure. CI runs every one, so adding the file is all it takes.

Tests run without a board, in Espressif's `espressif/idf:v6.1` Docker image (Linux, with bash and python3). So:

- Test the bridge, the hook and the scripts on their own: for example, start `hooks/butler-bridge.py serve` against a
  fake serial port, or feed `hooks/butler-hook.sh` a hook event on stdin and check what it sends.
- Things only a real board can show (the LED colours) can't be tested in CI. Describe what you checked by hand in the
  pull request instead.
- Don't touch the real `~/.claude/settings.json`, `~/.butler/` or serial ports. Point the scripts at temporary copies
  (`CLAUDE_SETTINGS`, `BUTLER_SERIAL`, `BUTLER_BRIDGE_PORT`, and a temporary `HOME`).

## Style

Write code that reads like the code around it: the same naming, comment style and structure. In the firmware, `main/`
only wires components together, and each concern lives in its own component under `firmware/components/`. Update
`README.md` when you change how something is used.

## License

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
