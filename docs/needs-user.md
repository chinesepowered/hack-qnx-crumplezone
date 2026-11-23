# Needs the owner

Only what cannot be done from the build session.

1. **Upload the video to YouTube** (public or unlisted-then-public, it is under 3 minutes):
   `demo/video.mp4`, thumbnail `demo/thumb.png`. Then put the YouTube link on the video line at the top of
   `README.md` and in `docs/devpost.md` (both say "YouTube link to be added").
2. **Make the GitHub repository public.** `chinesepowered/hack-qnx-crumplezone` is private today and the
   hackathon requires a public repository with an open-source license (AGPL-3.0 is in `LICENSE`).
3. **Submit on Devpost** (`qnx`, category Automotive & Mobility, and the bonus "Best use of QNX-specific
   Technologies"). Deadline: Tue Dec 15 2026, 11:00 AM PT. Text for every field is in `docs/devpost.md`;
   stills are in `docs/screenshots/`.
4. **Optional, recommended: run `make test` once on a Linux machine with KVM** (`/dev/kvm` present). The cloud
   build session had no KVM, so every number in the repository was measured under QEMU TCG, where the
   emulator and the shared host alone miss about 1 to 3% of 1 ms deadlines. On KVM, `make test` asserts zero
   brake and steering deadline misses under every fault; that assertion could not be exercised here.
5. **QNX license**: the session deactivated its activation at the end (the license allows two activated
   machines). To build again on another machine, activate there first:
   `~/qnxinstall/qnxsoftwarecenter/qnxsoftwarecenter_clt -syncLicenseKeys -myqnx.user=... -myqnx.password=... -activateAll`
   and deactivate with `-deactivateAll` when done.
6. **Keys**: the W&B Inference and ElevenLabs keys only ever lived in the git-ignored `.env`
   (`scripts/check-secrets.sh` scans the working tree and the full history for them). They were pasted into
   a chat to set up the session, so rotating them after the hackathon is good hygiene.
