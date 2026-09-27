# Contributing

Contributions are welcome. The driver is in beta and hardware-dependent;
keep changes focused and describe what was actually built or tested.

## License

Project contributions are under GPL-2.0-only; see [LICENSE](LICENSE).
Preserve upstream copyright and license notices and identify adapted code.

## Changes

- Explain the behavior being changed and the reason for it.
- Keep public documentation consistent with implemented and qualified behavior.
- Do not add vendor binaries, private research material, machine-specific logs,
  credentials, or personal host details.
- Preserve clear attribution and licensing information. Raise provenance
  questions before copying or adapting code whose origin is uncertain.

## Build and reports

For source-only changes, build against the target kernel headers with:

```sh
make -C src
```

For hardware changes, include the card and kernel details, exact input/output
mode and format, steps to reproduce, observed result, and whether the test was
repeated. Remove personal paths, IP addresses, usernames, serial numbers, and
other identifying data from logs and attachments. Never include secrets.

Hardware testing is optional for documentation-only changes. Do not describe
an untested code path as qualified.
