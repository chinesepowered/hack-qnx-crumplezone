# Narration

Voice: ElevenLabs Jessica (cgSgspJ2msm6clMCkdW9), model eleven_multilingual_v2.

- 0.6 s: Carmakers are moving infotainment, the instrument cluster and brake control onto one computer. A crashing media app must never delay a brake command. This is Crumple Zone, live on QNX OS 8.0. Watch the brake trace as we crash the infotainment system.
- 18.6 s: The brake loop never blinked. The guardian saw the segmentation fault, restarted infotainment, and had it back in 17.1 milliseconds.
- 28.0 s: Every number on screen is measured inside QNX. The brake loop runs at one kilohertz, FIFO priority sixty, pinned to its own core, and reads wheel speed from a resource manager through time-bounded message passing. Infotainment runs at priority eight, as an unprivileged user, with a memory limit.
- 48.3 s: Now a CPU storm. Infotainment spins a thread on every core, including the brake core, and all three CPUs hit one hundred percent. The brake loop simply preempts it, every period.
- 63.1 s: Next, a memory leak. Infotainment allocates until the limit the guardian set. Malloc fails, it aborts alone, and it is back in 8.7 milliseconds.
- 80.2 s: A message flood. Four threads hammer the instrument cluster. QNX queues messages by priority, so the flood is served at infotainment priority, and the cluster display stays fresh.
- 96.5 s: Now the hard case: we kill the wheel speed sensor driver. The brake loop notices on its next read, 3 milliseconds after the kill, and holds its last command. The guardian restarts the driver in 11.1 milliseconds, and normal control resumes.
- 116.3 s: After each run, Qwen on Weights and Biases Inference writes an incident report from the structured event log. Code checks every number against the log before it is shown, and all 20 numbers here were verified.
- 137.5 s: This is emulated hardware on a shared cloud host, and the dashboard shows that noise honestly; the kernel traces in the repository separate it from QNX scheduling. Process isolation, priorities, message passing and a guardian. The crash stays in the crumple zone.
