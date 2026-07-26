# **Cemu - Android Port (Wii U Emulator)**

This is the Android port of **Cemu**, a Wii U emulator written in C/C++.
It is still early and experimental. Stability, performance, and features are not guaranteed, and some functionality may be missing compared to the desktop version.

There is no timeline for when this port will be finished or when new features will be implemented.

**Please do not open issues or pull requests yet.**
Development is ongoing, and contributions will be welcome once the project is more stable.

For general information about Cemu, see the [official website](https://cemu.info) and [main repository](https://github.com/cemu-project/Cemu).

#### AI generated contributions:

We ask that all code submitted is written and understood by a human. You can use AI for planning, designing, reviewing and for asking questions about the codebase, but the code itself needs to be written by you. As a small exception you can use intellisense-style AI code autocompletion for pure boilerplate code as long as it's only a small part of your submission. To further clarify, when we ask for "human written" that excludes letting an AI write the code and then paraphrasing it. In other words, we are asking for human effort.

Why this policy exists:

We have relatively low reviewing capacity and requiring human-written code increases the quality and trustworthyness of submitted pull requests. There are also general concerns with AI usage in emulation:
- LLMs tend to make up solutions that work on the surface but are generally not accurate in the emulation sense
- There is evidence that LLMs have been trained on leaked proprietary SDKs and we cannot verify the origin of the knowledge. This is especially a problem for core emulation logic

Please keep these points in mind when contributing to Cemu. Contributions that do not follow this policy may be rejected.

## License
Cemu is licensed under [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.
