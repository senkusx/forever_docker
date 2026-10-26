"""AI providers the brain can use: Claude (Anthropic API), Ollama, and any OpenAI-compatible server
(llama.cpp server, vLLM, LM Studio, LocalAI, OpenRouter...). Each one turns (system, request, schema) into a dict."""

import json
import logging
import os
import re

import anthropic
import httpx

log = logging.getLogger("companions-brain")


class ProviderError(Exception):
    pass


def parse_json(text):
    """The answer as a dict; small local models sometimes wrap it in prose or a code fence."""
    text = (text or "").strip()
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", text, re.S)
        if not match:
            raise ProviderError(f"answer is not JSON: {text[:120]!r}")
        try:
            data = json.loads(match.group(0))
        except json.JSONDecodeError as e:
            raise ProviderError(f"answer is not JSON: {text[:120]!r}") from e
    if not isinstance(data, dict):
        raise ProviderError("answer is not a JSON object")
    return data


def options_of(row):
    try:
        options = json.loads(row.get("options") or "{}")
        return options if isinstance(options, dict) else {}
    except json.JSONDecodeError:
        return {}


class Anthropic:
    def __init__(self, row):
        self.model = row["model"] or "claude-opus-5-5"
        self.options = options_of(row)
        key = row.get("api_key") or os.environ.get("ANTHROPIC_API_KEY") or None
        self.client = anthropic.Anthropic(
            api_key=key,
            base_url=row.get("endpoint") or None,
            max_retries=1,
            timeout=float(row.get("timeout_s") or 30),
        )
        self.fallbacks = True

    def ask(self, system, request, schema):
        args = dict(
            model=self.model,
            max_tokens=int(self.options.get("max_tokens", 2048)),
            system=system,
            messages=[{"role": "user", "content": request}],
            output_config={
                "effort": self.options.get("effort", os.environ.get("COMPANIONS_LLM_EFFORT", "low")),
                "format": {"type": "json_schema", "schema": schema},
            },
        )
        try:
            response = None
            if self.fallbacks:
                try:
                    # on a policy decline the API retries on a fallback model in the same call
                    response = self.client.beta.messages.create(betas=["server-side-fallback-2026-07-01"], fallbacks="default", **args)
                except anthropic.BadRequestError as e:
                    if "fallback" not in str(e).lower():
                        raise
                    log.warning("server-side fallbacks unavailable, continuing without them: %s", e)
                    self.fallbacks = False
            if response is None:
                response = self.client.messages.create(**args)
        except anthropic.APIStatusError as e:
            raise ProviderError(f"API error {e.status_code}: {e.message}") from e
        except anthropic.APIConnectionError as e:
            raise ProviderError(f"cannot reach the API: {e}") from e

        if response.stop_reason == "refusal":
            return {"replies": []}
        if response.stop_reason == "max_tokens":
            raise ProviderError("answer cut at max_tokens")
        return parse_json(next((block.text for block in response.content if block.type == "text"), ""))


class Ollama:
    TOP_LEVEL = ("keep_alive", "think", "orders", "kinds")

    def __init__(self, row):
        self.model = row["model"]
        self.endpoint = (row.get("endpoint") or "http://localhost:11434").rstrip("/")
        self.options = options_of(row)
        self.timeout = float(row.get("timeout_s") or 30)
        self.headers = {"Authorization": f"Bearer {row['api_key']}"} if row.get("api_key") else {}

    def ask(self, system, request, schema):
        body = {
            "model": self.model,
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": request}],
            "stream": False,
            "format": schema,
            "keep_alive": self.options.get("keep_alive", "30m"),
            "options": {"temperature": 0.7, **{k: v for k, v in self.options.items() if k not in self.TOP_LEVEL}},
        }
        if "think" in self.options:
            body["think"] = self.options["think"]     # reasoning models (qwen3...): false answers fast
        try:
            response = httpx.post(f"{self.endpoint}/api/chat", json=body, headers=self.headers, timeout=self.timeout)
        except httpx.HTTPError as e:
            raise ProviderError(f"cannot reach Ollama at {self.endpoint}: {e}") from e
        if response.status_code != 200:
            raise ProviderError(f"Ollama {response.status_code}: {response.text[:200]}")
        return parse_json(response.json().get("message", {}).get("content", ""))


class OpenAICompatible:
    """POST {endpoint}/chat/completions. Structured output where the server supports it, else JSON mode, else plain."""

    FORMATS = ("json_schema", "json_object", "none")

    def __init__(self, row):
        self.model = row["model"]
        self.endpoint = (row.get("endpoint") or "http://localhost:8000/v1").rstrip("/")
        self.options = options_of(row)
        self.timeout = float(row.get("timeout_s") or 30)
        self.headers = {"Authorization": f"Bearer {row['api_key']}"} if row.get("api_key") else {}
        self.format = 0

    def ask(self, system, request, schema):
        while True:
            body = {
                "model": self.model,
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": request}],
                "max_tokens": int(self.options.get("max_tokens", 600)),
                "temperature": float(self.options.get("temperature", 0.7)),
            }
            kind = self.FORMATS[self.format]
            if kind == "json_schema":
                body["response_format"] = {"type": "json_schema", "json_schema": {"name": "companions", "schema": schema, "strict": True}}
            elif kind == "json_object":
                body["response_format"] = {"type": "json_object"}
            # fields this server wants in the request, e.g. {"enable_thinking": false} for Qwen 3 on Alibaba Model Studio
            extra = self.options.get("body")
            if isinstance(extra, dict):
                body.update(extra)
            try:
                response = httpx.post(f"{self.endpoint}/chat/completions", json=body, headers=self.headers, timeout=self.timeout)
            except httpx.HTTPError as e:
                raise ProviderError(f"cannot reach {self.endpoint}: {e}") from e
            if response.status_code in (400, 422) and self.format + 1 < len(self.FORMATS):
                # the server does not know this response format: try the next one
                log.info("%s: %s not supported, trying %s", self.endpoint, kind, self.FORMATS[self.format + 1])
                self.format += 1
                continue
            if response.status_code != 200:
                raise ProviderError(f"{response.status_code}: {response.text[:200]}")
            choices = response.json().get("choices") or [{}]
            return parse_json((choices[0].get("message") or {}).get("content", ""))


KINDS = {"anthropic": Anthropic, "ollama": Ollama, "openai": OpenAICompatible}


def make(row):
    kind = KINDS.get(row["kind"])
    if not kind:
        raise ProviderError(f"unknown provider kind {row['kind']!r}")
    return kind(row)
