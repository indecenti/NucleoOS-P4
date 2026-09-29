<!-- All PRs require review and approval from @indecenti before merge. -->

## What & why
<!-- What does this change and why? Link the related issue. -->

## Testing
<!-- How did you verify it? (idf.py build, on-device, screenshots) -->

## Checklist
- [ ] Builds with ESP-IDF v5.5.2 (`idf.py build`, target `esp32p4`)
- [ ] CI green: clean-checkout firmware build, memory budgets (`tools/ci/check_budgets.py`), host tests + fuzzers
- [ ] Parses SD/LAN input? It lives in a pure module with a unit test and a fuzzer in `tests/host`
- [ ] No new claims that aren't actually verified on hardware
- [ ] I read and agree to [CONTRIBUTING.md](../CONTRIBUTING.md) (contributor license grant)
- [ ] Any new third-party code is listed in [THIRD_PARTY.md](../THIRD_PARTY.md)
