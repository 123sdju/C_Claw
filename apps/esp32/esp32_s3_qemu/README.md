# c-claw ESP32-S3 QEMU Smoke Test

This ESP-IDF project builds `c_claw_core` for `esp32s3` and runs a small
QEMU smoke test plus an optional UART chat loop. The default smoke profile
stays small:

- no CLI gateway
- no shell/plugin/Docker
- no SQLite
- no HTTP tool or LLM providers unless `chat-real` is selected
- file tools remain compiled against ESP-IDF VFS

The test checks core portability basics: JSON, structured messages,
FreeRTOS-backed thread/mutex, the tool registry, and a writable
`/sdcard/cclaw/workspace`.

The board also registers a board-local ESP32-only `gpio` tool from
`main/tools/cc_esp32_gpio_tool.c`. It supports
`read`, `write`, and `toggle` operations against a conservative allowlist
of GPIO pins, so the agent can drive simple IO without exposing UART pins
or flash-related pins.

Board hardware tools live with the board instead of `adapters` or
`cclaw/platforms/esp32`. For STM32 or another ESP32 app, add an app-local
tool such as `apps/stm32/<board>/main/tools/cc_stm32_gpio_tool.c` and
register it in that board's feature set rather than reusing this QEMU GPIO
policy.

QEMU note: Espressif's `esp32s3` machine does not expose an SD/MMC bus, so
this example mounts a FAT data partition named `sdcard` at `/sdcard`. That
keeps the VFS path and file behavior close to a real SD card. On physical
ESP32-S3 hardware, keep the `/sdcard` path and replace the mount code with
SDMMC or SDSPI.

## Prerequisites

Install ESP-IDF and the Espressif QEMU binaries, then export the IDF
environment:

```bash
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa qemu-riscv32
. "$IDF_PATH/export.sh"
```

## Run

From the repository root:

```bash
./scripts/esp32_s3_qemu.sh doctor
./scripts/esp32_s3_qemu.sh build
./scripts/esp32_s3_qemu.sh qemu
```

The script writes ESP-IDF build output to `build/app/esp32/esp32_s3_qemu` by default.
Override it with `CCLAW_ESP32S3_QEMU_BUILD_DIR` when a separate build tree is
needed.

Generated ESP-IDF files stay in the build tree. The source directory keeps only
`sdkconfig.defaults`; the generated configuration is written to:

```text
build/app/esp32/esp32_s3_qemu/sdkconfig
```

The QEMU command succeeds when the UART log contains:

```text
CCLAW_QEMU_PASS
```

`qemu` is a smoke-test command: it rebuilds the image, starts QEMU, captures the
UART log, and treats `CCLAW_QEMU_PASS` as success. The board then enters the
mock UART chat loop, so the process may continue until the configured timeout
expires. Set `CCLAW_QEMU_TIMEOUT_SECONDS` lower for quick CI smoke checks.

Current reference firmware size for the default smoke profile:

```text
c_claw_esp32_s3_qemu.bin  272,800 bytes, about 266.4 KiB
factory app partition     1 MiB, about 757.6 KiB free
```

For interactive debugging:

```bash
./scripts/esp32_s3_qemu.sh monitor
./scripts/esp32_s3_qemu.sh gdb
```

## Interactive Chat

Run the local mock chat loop:

```bash
./scripts/esp32_s3_qemu.sh chat
```

Run real OpenAI-compatible chat over QEMU Ethernet:

```bash
./scripts/esp32_s3_qemu.sh chat-real
```

`chat-real` reads `model.api_key`, `model.base_url`, and `model.model` from
`apps/esp32/esp32_s3_qemu/config/config.json` when the environment is not set.
Inside the emulated board, runtime defaults point at `/sdcard/cclaw/config.json`,
`/sdcard/cclaw/data`, and `/sdcard/cclaw/workspace`. You can also
override them explicitly:

```bash
export CCLAW_QEMU_LLM_API_KEY='...'
export CCLAW_QEMU_LLM_BASE_URL='https://api.openai.com'
export CCLAW_QEMU_LLM_MODEL='gpt-4o-mini'
./scripts/esp32_s3_qemu.sh chat-real
```

When the prompt shows `you>`, type a message and press Enter. Use `/exit` to
stop the chat loop, then press `Ctrl-A` followed by `X` to close QEMU.

Example LLM tool arguments for GPIO control:

```json
{"operation":"write","pin":2,"level":1}
```

```json
{"operation":"read","pin":2}
```
