# c-claw STM32H743I-EVAL Renode FreeRTOS Smoke

This target builds a small FreeRTOS application for the STM32H743I-EVAL software
profile and runs it in Renode using the STM32H743 CPU platform description.

The smoke checks core portability basics: JSON parsing and the generic
FreeRTOS-backed c-claw thread/mutex layer. It succeeds when the UART log
contains:

```text
CCLAW_STM32H743_RENODE_PASS
```

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

The API key is never printed by the script, but it is embedded in the generated
build header and ELF for real tests.
