# Femto voice backend.
FROM python:3.12-slim

# uv — the Python package/dependency manager. Pinned to a minor version:
# a moving tag would invalidate the cached dependency layer below.
COPY --from=ghcr.io/astral-sh/uv:0.10 /uv /uvx /bin/

ENV UV_LINK_MODE=copy \
    UV_COMPILE_BYTECODE=1 \
    HTTP_PORT=7860

WORKDIR /app

# Install dependencies first so this layer is cached across code changes.
COPY pyproject.toml uv.lock ./
RUN uv sync --no-dev --no-install-project --locked

# Application code. Glob *.py so adding new modules doesn't silently break
# the image (tests/ is a subdir and is not matched by *.py at the top level).
COPY *.py *.html ./

EXPOSE 7860

CMD ["uv", "run", "--no-dev", "python", "bot.py"]
