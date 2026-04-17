# Agent instructions

## Coding style

- Follow the Nginx core development guide https://nginx.org/en/docs/dev/development_guide.html

## Verifying changes

- Follow the Development section in [README.md](README.md): use the included development container for development, testing, and manual validation work.
- Do not run `make` commands on the host machine.
- When a task requires `make`, run it inside the `dev` container instead.
- Prefer one of these patterns:
  - `docker compose run --rm dev make <target>` for one-off commands.
  - `make shell` first, then run `make <target>` from inside the container for multi-step interactive work.

## README hygiene

After any change that affects user-visible behaviour, known limitations, configuration options, or the public API, **review `README.md` and update it** to reflect the current state before closing the task. Pay particular attention to the **Known issues** section, which documents known limitations — keep each entry accurate and up to date.
