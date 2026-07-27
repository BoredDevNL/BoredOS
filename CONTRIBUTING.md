# Contributing to BoredOS

First off, thank you for considering contributing to BoredOS! Building an operating system from scratch is a massive undertaking, and community contributions are what make it possible.

This document outlines the guidelines, workflows, and standards for contributing to our repositories.


## 1. Code of Conduct

We aim to foster an open, welcoming, and respectful community. Please treat all contributors with respect, deliver constructive feedback, and keep discussions focused on technical merit.


## 2. Getting Started & Where to Help

Before jumping into code:
* **Check the Issue Tracker:** Look for open issues labeled `good first issue` or `help wanted`.
* **Join the Discussion:** Join our [Discord Server](https://discord.gg/J2BxWaFAgY) to discuss architecture or proposed features before spending hours writing code.
* **Propose Big Changes First:** For major kernel features, driver architecture overhauls, or breaking API changes, please open an **RFC (Request for Comments) Issue** first so we can align on design or ask in the [Discord Server](https://discord.gg/J2BxWaFAgY).


## 3. Development Workflow

1. **Fork the Repository** and create your branch from `main`:

   `git checkout -b feature/my-cool-driver`
2. **Build & Test Locally:** Ensure your changes build cleanly without warnings.
3. **Keep Commits Clean:** 

   Write clear, structured commit messages detailing *what* changed and *why*. We follow the **Conventional Commits** standard (`type(scope): description`). 

   Please prefix your commit messages (and PR titles) with one or similar of the following types:
   - `feat:` 
   
   A new feature or subsystem (e.g., `feat(sched): add priority queue scheduler`)

   - `fix:` 
   
   A bug fix (e.g., `fix(mem): resolve double-free in page allocator`)

   - `refactor:`
   
    Code restructuring without changing functionality or fixing bugs
   - `docs:`
   
    Documentation changes only
   - `ci:` 
   
   Changes to build scripts, toolchains, or GitHub Actions
   - `test:` 

   Adding or updating kernel tests



   *Keep commits focused, atomic, and logically grouped. Avoid mixing unrelated changes into a single commit.*

---

## 4. Legal & Security Safeguards

To keep our codebase healthy, secure, and legally safe under the **GPLv3 license**, we enforce three automated checks on all Pull Requests:

### A. Developer Certificate of Origin (DCO)
We use the DCO (the same system used by the Linux Kernel). By signing off on your commits, you certify that you wrote the code or have the right to contribute it under GPLv3.

Simply add the `-s` flag when committing:
git commit -s -m "kernel/mem: fix double free in page allocator"

### B. Cryptographic Commit Signing
To help prevent identity spoofing, we encourage signing commits with an **SSH or GPG key** attached to your GitHub account. While not strictly required to merge a PR, it is highly recommended!
# Example: commit with signing and DCO

`git commit -S -s -m "your commit message"`

*If you choose not to sign with an SSH/GPG key, simply remove the `-s` flag*

<details>
<summary>How to set up SSH Commit Signing with GitHub (Click to expand)</summary>

Setting up SSH signing takes less than 2 minutes using an existing SSH key:

1. **Check if you have an SSH key:**  
   Run this in your terminal: `cat ~/.ssh/id_ed25519.pub`  

   If you don't have one, generate it with:
   
   `ssh-keygen -t ed25519 -C "your_email@example.com"`

   *(Press ENTER when asked for a passphrase if you prefer no password prompt, or enter a passphrase for extra security)*

2. **Add the key to GitHub:**  
   Copy the key contents, go to **GitHub Settings -> SSH and GPG keys -> New SSH Key**, choose **Key type: Signing Key**, and paste it.

3. **Configure Git locally:**  
   Run these 3 commands in your terminal to enable automatic SSH signing for all your commits:

   ```bash
   git config --global commit.gpgsign true
   git config --global gpg.format ssh
   git config --global user.signingkey ~/.ssh/id_ed25519.pub
</details>

<details>
<summary>How to auto-sign all commits with DCO (Click to expand)</summary>

Tired of typing `-s` on every commit? You can set up a global Git hook to automatically sign off every commit!

```bash
mkdir -p ~/.git-templates/hooks
git config --global init.templatedir '~/.git-templates'

cat << 'EOF' > ~/.git-templates/hooks/prepare-commit-msg
#!/bin/sh
NAME=$(git config user.name)
EMAIL=$(git config user.email)

if [ -n "$NAME" ] && [ -n "$EMAIL" ]; then
    SIG=$(printf "Signed-off-by: %s <%s>" "$NAME" "$EMAIL")
    if ! grep -qG "^$SIG" "$1"; then
        printf "\n%s\n" "$SIG" >> "$1"
    fi
fi
EOF
chmod +x ~/.git-templates/hooks/prepare-commit-msg
```
This will automatically apply to all newly cloned repositories. For any existing repos on your machine, simply run git init inside the project folder once to activate it!

</details>

## 5. AI-Generated Code Policy
While AI can assist with drafting documentation or overcoming language barriers, **we prefer human-written code.** BoredOS deals with low-level kernel and hardware interactions where AI code often introduces system crashes, hardware bugs, or sloppy architecture.

* **Code Contributions:** Using AI to generate code is heavily discouraged. If used, you **must** explicitly disclose it in your PR description, explain why/how it was used, and thoroughly audit the code yourself. Unverified or sloppy AI code will be closed and tagged as `AI Slop`.
* **Docs & Comments:** AI tools may be used to assist with documentation or issue discussions, provided the content is reviewed by a human for accuracy and disclosed.

**Read our full [AI Policy](AI-POLICY.md) for detailed guidelines, expectations, and policy violation terms.**

## 6. Coding Standards & Style Guide

Operating system code requires extreme attention to detail. Please adhere to these guidelines:

* **Memory Safety & Leaks:** 

Always verify pointer bounds, check allocation failures, and cleanup resources properly.
* **Documentation:** 

Comment complex, hard to read, hardware-specific quirks, memory layouts, or concurrency primitives clearly.


## 7. Submitting a Pull Request (PR)

When your code is ready:
1. Push your branch to your fork and submit a PR against `main`.
2. Fill out the PR template, describing **what problem this solves** and **how you tested it.**
3. Ensure all CI checks pass
4. Address review feedback constructively. Once approved, a maintainer will merge your PR!

**Thank you for helping make BoredOS better!**
