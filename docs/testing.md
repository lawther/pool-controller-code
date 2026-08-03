# Tests

Everything here runs on the host — no ESP32 required. The suite has two halves:

- **Unit tests** — each `test/test_*.c` is compiled against the module it names (`test_foo.c` builds with `../main/foo.c`) and run directly.
- **Replay tests** — captured bus logs are fed back through `message_decoder.c` and the decoder's output is diffed against what the capture recorded. Each `RX MSG: <hex>` line in a sample is passed to `decode_message()`, and the resulting ESP_LOG output is compared with the `MSG_DECODER:` lines that follow it in the file (timestamps ignored). This catches behaviour drift between a captured trace and the current decoder.

## Running

```bash
bash test/run_tests.sh
```

Builds and runs every `test/test_*.c` (except the skip list — see the top of `run_tests.sh`), then replays every `*.txt` under `test/samples/`.

From VS Code inside the devcontainer: **Cmd+Shift+P → "Tasks: Run Task"** lists:

- **Run Tests** — full suite (unit + replay).
- **Replay: all samples** — just the replay step.
- **Replay: bless all samples** — rewrite expected output in every sample using the current decoder's output. Use after intentional decoder/logging changes, then review `git diff test/samples/` before committing.
- **Replay: single file** / **Replay: bless single file** — same but prompted for a single path.
- **Framing: run** — exercise the sliding-window frame parser against the goldens in `test/frames/`.
- **Framing: bless goldens** — regenerate `test/frames/*_output.txt` from the current parser. Use after intentional framing changes, then review `git diff test/frames/` before committing.

## Adding a regression sample

1. Capture bus traffic into a log file (e.g. via the TCP debug port or `idf.py monitor`).
2. Drop the file into `test/samples/`.
3. Run **Replay: bless single file** against it — this normalises the expected output to the current decoder.
4. Review with `git diff`, then commit. The file is now a regression test.

## Adding a unit test

Name the file after the module it exercises — `test_foo.c` is compiled with `../main/foo.c` automatically, no build config to edit. Follow the existing pattern in `test/test_message_decoder.c`:

```c
void test_my_feature(void)
{
    // Setup
    memset(&test_pool_state, 0, sizeof(test_pool_state));
    test_ctx.pool_state = &test_pool_state;
    test_ctx.enable_mqtt = false;

    // Create test message
    uint8_t msg[] = { /* your message bytes */ };

    // Execute
    bool result = decode_message(msg, sizeof(msg), &test_ctx);

    // Assert
    TEST_ASSERT(result, "Message should be decoded");
    TEST_ASSERT(test_pool_state.some_field == expected_value, "Field should match");
}
```

Then call it from `main()`.

`run_tests.sh` has a `SKIP_LIST` at the top for tests broken by upstream API drift. Entries there are tech debt — re-enable each one once its test matches the current interfaces, rather than leaving it excluded permanently.

## Mocking

`test/` supplies mock headers for FreeRTOS and ESP-IDF logging, which is what lets the decoder compile and run without the toolchain:

- `xTaskGetTickCount()` — returns a fixed value
- `xSemaphoreTake()` / `xSemaphoreGive()` — no-ops that always succeed
- `mqtt_publish_*()` — empty stubs; MQTT is disabled per-test via `enable_mqtt = false`

## Frame parser (sliding window) tests

The frame parser is tested separately from the decoder: each line of `test/frames/observed_error_frames.txt` (real bus captures) and `test/frames/synthetic_errors.txt` (synthetic, one per failure mode) is fed to the parser, and the ordered stream of resync events (`Resync:  <type>`) and decoded frames is diffed against the matching `--- Case N ---` block in the `*_output.txt` golden. On a mismatch the failure prints the source location as `path:line`.

The goldens are blessable from the CLI, mirroring replay:

```bash
cd test
gcc -I. -I.. -o run_framing test_framing.c log_capture.c && ./run_framing --bless; rm -f run_framing
```

After blessing, review `git diff test/frames/` — the diff is the exact record of how parser behaviour changed.

## Improving "Unhandled" message logging

When the decoder gains support for a previously-unknown command, replaying old samples surfaces it as a mismatch (e.g. `Unhandled CMD=0xXX` → real decode). Bless the affected samples and the diff in git is the exact documentation of what improved.
