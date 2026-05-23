# c-claw STM32H743I-EVAL Renode FreeRTOS Target

This target builds a small FreeRTOS application for the STM32H743I-EVAL software
profile and runs it in Renode using the STM32H743 CPU platform description.

The smoke checks core portability basics: JSON parsing and the generic
FreeRTOS-backed c-claw thread/mutex layer. It succeeds when the UART log
contains:

```text
CCLAW_STM32H743_RENODE_PASS
```

When SD/FatFs is enabled, the firmware mounts the attached card as the c-claw
workspace:

```text
/sdcard/cclaw/workspace
```

The STM32 profile enables c-claw file tools against that workspace path.

## Prerequisites

- `arm-none-eabi-gcc`
- Renode portable at `~/opt/renode` or `RENODE_PATH` pointing to `renode`
- FreeRTOS-Kernel at `~/opt/FreeRTOS-Kernel` or `FREERTOS_KERNEL_PATH`
- STM32CubeH7 at `~/opt/STM32CubeH7` or `STM32CUBEH7_PATH`
- For real network tests: `apps/posix/cli/config/config.json` with an
  OpenAI-compatible `model.base_url`, `model.model`, and `model.api_key`

## Run

From the repository root:

```bash
./scripts/stm32h743_renode.sh install-renode
./scripts/stm32h743_renode.sh install-freertos
./scripts/stm32h743_renode.sh doctor
./scripts/stm32h743_renode.sh build
./scripts/stm32h743_renode.sh renode
```

Network and real LLM smoke tests:

```bash
./scripts/stm32h743_renode.sh net
./scripts/stm32h743_renode.sh http
./scripts/stm32h743_renode.sh real-tls
./scripts/stm32h743_renode.sh stress
```

Interactive terminal-as-UART chat:

```bash
./scripts/stm32h743_renode.sh chat
```

The chat command builds firmware with the real LLM config injected into the
build directory, configures TAP/NAT, starts Renode, and connects the current
terminal to USART3. Type a line and press Enter to send it. Use `/help` for
firmware-side commands, `/quit` to stop the chat task, and ESC to leave
Renode's UART connection mode.

The chat path uses a generated UTF-8 Renode monitor command, so prompts typed
in Chinese are sent to the STM32 as UTF-8 bytes instead of Unicode codepoints.

By default the script also prepares and attaches a Renode SD-card image at:

```text
build/app/stm32/stm32h743i_eval_renode/workspace-sd.img
```

By default the script creates an empty FAT workspace and does not copy the
repository into the image. Set `CCLAW_STM32H743_SKIP_SD_COPY=0` and
`CCLAW_STM32H743_WORKSPACE_DIR=/path/to/small/workspace` to seed files into
`/sdcard/cclaw/workspace`. Install `dosfstools` and `mtools` if you want the
script to format the image as FAT and copy workspace files into it. Without
those tools it still creates an empty block image for Renode attachment.

Firmware UART chat exposes quick file commands backed by the same FatFs c-claw
filesystem implementation:

```text
/ls [path]
/cat <path>
/write <path> <text>
```

The API key is never printed by the script, but it is embedded in the generated
build header and ELF for real tests.
