# Companions brain: AI workers for the bots (Claude, Ollama, OpenAI-compatible servers).
# Next to the server: docker compose --profile brain up -d
# On another machine: docker run -d -e BRAIN_URL=https://your.site -e BRAIN_TOKEN=<token> <this image>
FROM python:3.12-slim

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt \
    && useradd --system --uid 10001 brain

COPY brain.py backends.py /app/
USER brain
WORKDIR /app
CMD ["python", "-u", "/app/brain.py"]
