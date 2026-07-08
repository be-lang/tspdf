<!-- Thanks for contributing! Please read CONTRIBUTING.md first. -->

## What this changes
Briefly describe the change and why.

## How it was verified
- [ ] `make check` passes locally (clean `-Werror` rebuild + full suite)
- [ ] Added or updated tests (test-first where practical)
- [ ] For parser changes: ran the relevant fuzzer (`make fuzz`) and added any
      reproducer to `fuzz/corpus/`
- [ ] For output changes: validated with `make test-external` (qpdf/mutool)
- [ ] Updated `CHANGELOG.md` (`[Unreleased]`) for user-visible changes

## Notes
- [ ] No new external dependencies
- Anything reviewers should pay special attention to:
