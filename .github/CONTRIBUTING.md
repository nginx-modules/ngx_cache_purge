# Contributing Guidelines

Thank you for contributing! To maintain a clean workflow, please follow our branching and PR rules.

---

## 📌 Branching Model & Workflow

* **`master` Branch:** 
  All active development, new features (currently targeting **v4**), and major updates must be submitted against `master`.
* **Release / Maintenance Branches:** 
  Older versions are frozen into dedicated release branches:
  * **`releases/3.0`** (v3.0.x releases)
  * **`releases/2.5`** (v2.5.x releases)
  
* **Backport Policy:**
  * Features are implemented and stabilized on `master` first.
  * Backporting to maintenance branches (`releases/3.0` or `releases/2.5`) is evaluated and handled after landing on `master`.
  * **Do not submit new feature PRs directly to release branches** without prior maintainer approval.

---

## 🛠 Submitting Pull Requests

1. **Use Draft PRs for Work in Progress:** 
   If your PR is an unfinished draft, experimental, or created for testing, please open it as a **Draft Pull Request**.
2. **Ready for Review:** 
   Only mark your PR as **"Ready for Review"** when code is complete and all CI checks pass.
3. **CI Requirement:** 
   Maintainers will only review PRs that pass all GitHub Actions / CI checks.
4. **Merging & Credits:** 
   Maintainers may push minor fixes or cleanup commits directly to your PR branch prior to merging.
   PRs are typically merged using **Squash and Merge**, and contributor credits will be reflected in the release notes upon official tag publication.
