<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Troubleshooting

This page tells users how to fix known problems with PlasmaZones and how to attach a support report to a bug. The
**[Troubleshooting](https://phosphor-works.org/plasmazones/troubleshooting/)** page on the website has more: daemon
startup, verbose logging, KWin minimum-size rules and the full support-report procedure.

## The screen looks blurred all the time with Better Blur DX

The force-blur mode of Better Blur DX blurs every window that is not on its allowlist. This includes the overlay
surfaces of PlasmaZones, so the screen looks blurred all the time.

1. Open **System Settings → Window Management → Desktop Effects → Better Blur DX → Configure**.
2. Open the **Force blur** tab.
3. Select **"Blur all except matching"**.
4. Add this line:

```text
/^plasmazones/
```

The slashes are necessary. They put the parser into regex mode. One entry covers the daemon, the editor and the
settings app. The "Blur all except matching" setting is the important part. In the default mode, the list is a list
of windows to blur, not a list of exclusions.

## Attach a support report to a bug

When you file a bug, attach a support report:

```bash
plasmazones-report
```

By default, the archive goes into `/tmp`, and the report removes the paths of your home directory. Thus you can
safely attach it to a public issue.

## See also

- [Use](USAGE.md)
- [Install](INSTALL.md)
