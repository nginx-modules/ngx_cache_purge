# Security Policy

## Supported Versions

We actively maintain and provide security patches for the following versions of `ngx_cache_purge`. If you are running an older or unsupported version, we strongly recommend upgrading to the latest stable release.

| Version | Supported          |
| :--- | :---: |
| `4.x.x` / `4.x` | :white_check_mark: |
| `3.x.x` / `3.x` | :white_check_mark: |
| `2.5.x` / `2.5` | :white_check_mark: |
| `< 2.5`         | :x:                |

---

## Reporting a Vulnerability

The `ngx_cache_purge` module deals with low-level NGINX shared memory, cache key lookups, file path boundary checks, and custom memory management. Because security issues (such as buffer overflows, memory corruption, lock-contention flaws, or denial-of-service vectors) can directly impact production environments, **please do not open public issues, discussions, or pull requests for security vulnerabilities**.

Instead, please report security concerns privately through our secure reporting channel:

### GitHub Private Vulnerability Reporting (Recommended)
1. Go to the repository's [Security Advisories page](https://github.com/nginx-modules/ngx_cache_purge/security/advisories).
2. Click **Report a vulnerability** to open a private draft directly with the maintainers.
3. Provide a clear description of the vulnerability, affected components or version tags (e.g., path traversal checks, invalidation queues, or memory routines), along with steps to reproduce or a proof-of-concept (PoC).

---

## Disclosure Policy

When you report a security issue, we adhere to a coordinated disclosure process:
* **Acknowledgment:** We aim to acknowledge receipt of your private report within **48 hours**.
* **Investigation & Triage:** We will verify the issue against our test suite (`/t/`) and target build platforms.
* **Remediation & Release:** We will develop a secure patch, coordinate release timing with you, and publish a GitHub Security Advisory simultaneously with the updated stable release.

Thank you for helping keep `ngx_cache_purge` and the broader NGINX ecosystem secure!
