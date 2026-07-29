# Security Policy

## Reporting a Vulnerability

Discovered vulnerability can be directly reported using the [Security and quality](https://github.com/BoredOS/BoredOS/security) tab on our GitHub repository. They will be reviewed as soon as possible.

Malware that doesn't exploit omission in the kernel code base or bypasses system functions is not counted as vulnerability.[^1]

**Please do not report security vulnerabilities through public issues or discussions**

Please include as much of the information listed below as you can to help us better understand and resolve the issue:

- The type of issue (e.g., privilege level escalation, memory disclosure)
- Full paths of source file(s) related to the manifestation of the issue (if you have one)
- The location of the affected source code (tag/branch/commit or direct URL)
- Any special configuration required to reproduce the issue
- Step-by-step instructions to reproduce the issue
- Proof-of-concept or exploit code
- Impact of the issue, including how an attacker might exploit the issue

---

[^1]: A kernel vulnerability is a security flaw in the core operating system software that allows attackers to gain high-level system control, bypass security controls, crash the machine or do actions that program shouldn't be able to do.
