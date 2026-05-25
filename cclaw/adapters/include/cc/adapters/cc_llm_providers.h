/*
 * Public factories for the built-in HTTP-backed LLM providers.
 *
 * Applications wire these factories into cc_runtime_feature_set_t. The SDK
 * owns the transport and protocol adapters; applications still decide which
 * provider names are accepted by configuration.
 */
#ifndef CC_LLM_PROVIDERS_H
#define CC_LLM_PROVIDERS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"

cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

cc_result_t cc_ollama_provider_create(
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);

cc_result_t cc_anthropic_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

#endif
