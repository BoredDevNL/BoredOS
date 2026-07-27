# AI Policy

## Overview

This policy applies to all contributions and reviews (code, docs, PRs, issues) which were assisted or generated with AI tools. The goal is to keep product quality high, respect licensing, and maintain user trust. Meanwhile, AI tools are discouraged for use in generating code, but they can be used for documentation and issue comments with proper disclosure.

## Our Preference Is for Human Code

While we understand that AI tools can be helpful for generating code, we prefer that code contributions are primarily created by human developers. BoredOS relies on drivers and hardware that require careful consideration of performance, security, and maintainability. Human developers are better equipped to understand these factors and make informed decisions when writing code.

AI-generated code may not always meet our standards for quality and may introduce unforeseen issues. This may include, but is not limited to:

- Dysfunctional drivers
- System crashes
- Data loss

## Licensing & Provenance
 
BoredOS is licensed under GPLv3, and every contribution must be something the contributor has the right to submit under that license. This requirement does not change when AI tools are involved, but it does become harder to verify, so this section spells out how it applies.
 
**AI use extends your DCO certification.** When you sign off on a commit (`git commit -s`), you are certifying that you have the right to contribute the work under GPLv3. If any part of that commit was generated or substantially assisted by an AI tool, your sign-off also certifies that:
 
- To the best of your knowledge, the output does not reproduce copyrighted, proprietary, or incompatibly-licensed code from the tool's training data.
- You have reviewed the relevant AI tool's terms of service and believe the output is one you are entitled to license under GPLv3.
- You take the same authorship responsibility for the contribution as you would if you had written it yourself, line by line.
**Disclosures must name the tool.** "I used AI" is not sufficient. Disclose which tool (and version, if known) was used, since different tools carry different licensing risk depending on their training data and terms of service. This also gives maintainers something concrete to act on if a provenance concern comes up later.
 
**Provenance issues can surface after merge.** Licensing problems in AI-generated code are not always visible at review time. If a contribution is later found to reproduce copyrighted or incompatibly-licensed material, it will be reverted regardless of prior disclosure or merge status, and the contributor will be notified. Disclosure at submission time is still required and still matters for how a violation is handled, but it does not retroactively clear code that turns out to carry a real licensing problem.
 
**Maintainers may request additional verification.** For substantial AI-assisted contributions, especially in drivers or kernel code, maintainers may ask a contributor to run license/duplication-detection tooling (e.g., ScanCode Toolkit) against the submission, or may run it themselves, before merging.

## Pull Requests and Code Reviews

You are discouraged from using AI tools to generate code for pull requests. If AI tools are used, the contributor must disclose this in the PR description and ensure that the code meets our quality standards, explaining exactly **why** and **how** the AI tool was used.

We reserve the right to refuse any PR requests that look sloppy or generated using AI. The PR will be marked as `AI Slop`. We prefer someone who is honest about using AI tools and is willing to put in the effort to review and improve the code, rather than someone who tries to hide it.

We want to encourage transparency and accountability in our contributions. We believe that human-generated code is more likely to meet our standards for quality and maintainability.

## Documentation and Issue Comments

We understand that AI tools can be helpful due to language barriers. If AI tools are used for these purposes, the contributor must disclose this in the relevant section of the documentation or comment.

The content must be reviewed by a human to ensure accuracy and clarity before being published.

## Our Right

We reserve the right to reject or close AI-generated PRs at our discretion, just as we do any PR for any reason, including but not limited to the use of AI-generated code.

No PR is entitled to review regardless of its medium of creation.

## Violations

If a contributor is found to have violated this policy, the following actions may be taken:

- The contribution may be rejected or removed.
- The contributor may receive a warning or be temporarily banned from contributing.
- In severe cases, the contributor may be permanently banned from contributing.

## Conclusion

BoredOS is committed to maintaining high standards of quality and integrity in our contributions. We encourage the responsible use of AI tools while prioritizing human creativity and expertise.

By adhering to this policy, we can ensure that our project continues to thrive and deliver value to our users.
