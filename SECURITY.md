# Security Policy

This is a hobbyist home-automation firmware project maintained in spare time.
There is **no formal SLA** for security fixes, but reports are taken seriously
and will be addressed on a best-effort basis.

## Reporting a Vulnerability

Please **do not** open a public issue for security bugs.

Instead, report privately via one of:

- **GitHub Security Advisories** — go to the repository's **Security** tab and
  choose **Report a vulnerability**.
- **Email** — [greg@nullmethod.com](mailto:greg@nullmethod.com).

Please include enough detail to reproduce the issue (affected version / git
SHA, hardware, and steps). You'll get an acknowledgement as soon as practical.

## Security-Relevant Surfaces

This firmware is intended to run on a trusted home LAN. Be aware of the
following, which are relevant when assessing risk:

- **Stored credentials.** The device persists the OpenSprinkler device
  password in NVS (non-volatile storage). It is sent to the controller as an
  MD5 hash (`pw` parameter), per the OpenSprinkler HTTP API — this is a
  property of that API, not additional protection. An optional OTA update
  password may also be stored in NVS.
- **First-boot configuration.** A [WiFiManager](https://github.com/tzapu/WiFiManager)
  captive portal is used for initial WiFi and device configuration. Anyone able
  to associate with the setup access point during first boot can configure the
  device.
- **Optional debug interfaces.** Diagnostic builds may expose an HTTP
  screenshot server and a plaintext TCP log sink. These are **intended for LAN
  use only**, are unauthenticated, and should not be exposed to the public
  internet.

Because the device trusts the local network, treat LAN access as the primary
trust boundary. Do not port-forward the device or its debug interfaces.
