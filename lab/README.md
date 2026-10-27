# TX bench configuration

The TX tools refuse to transmit unless this directory contains a local
`test-setup.conf` with `cabled=yes` and a non-empty allowed-channel list.
Nothing here is wired to an antenna by default.

Copy the templates after the cabled bench is physically set up:

```sh
cp lab/test-setup.conf.example lab/test-setup.conf
cp lab/allowed_channels.txt.example lab/allowed_channels.txt
```

`test-setup.conf` keys:

- `cabled=yes` — only when the TX ports are coax-cabled through the
  attenuators to the RX adapter and no antenna is attached.
- `tx_adapter=Bus NNN.Port NNN` — the TX adapter's USB bus/port.
- `rx_adapter=...` — informational; the RX probe is selected explicitly.
- `attenuation_db=N` — actual in-line attenuation.
- `tx_paths=A|B|AB` — cabled TX paths.

`allowed_channels.txt` accepts comma- or whitespace-separated channels;
comments after `#` are ignored.

These files are intentionally not committed with a live `cabled=yes` value.
