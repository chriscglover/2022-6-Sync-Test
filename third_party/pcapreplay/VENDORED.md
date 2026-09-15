# Vendored from PCAP Replay

Source: PCAP Replay version 1.8.1, commit `eb619c5`. MIT licence, see `LICENSE`.

The files are copied unmodified:

- `common/`: the SDI format table, 10-bit packing (plus AVX2), the CRC,
  `SdiFrameBuilder` and `SdiStreamParser`, the HBRMT/RTP headers, interface
  enumeration, the multicast sender and receiver, the spin pacer, and platform
  shims.
- `nmos/`: the built-in IS-04/IS-05 node, DNS-SD, the HTTP client and server,
  and JSON.

The pcap reader, pcap source, replay engine, stats server, settings and the
Windows GUI are not needed and were left out.

To update, copy the same files from a newer PCAP Replay and run `make test`.
