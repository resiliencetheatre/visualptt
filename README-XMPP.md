# VisualPTT XMPP sender

`visualptt-xmpp-send` is an optional, standalone exporter. It stream-copies the
H.264 video from an existing VisualPTT MKV, converts its Opus audio to 48 kbit/s
AAC, uploads the resulting MP4 using XEP-0363, and sends its HTTPS URL in a
normal one-to-one XMPP message. It does not change recording or playback.

## Dependencies and compilation

On Debian 13 (Trixie):

```sh
sudo apt install build-essential pkg-config ffmpeg libstrophe-dev \
  libcurl4-openssl-dev libssl-dev
make
```

The normal `make` target builds `visualptt-xmpp-send` with the other programs.
libstrophe supplies the XMPP client, certificate-verified TLS and SASL; libcurl
supplies the certificate-verified HTTPS PUT client.

## Configuration

Create `~/.config/visualptt/xmpp.ini`:

```ini
[xmpp]
jid=visualptt@example.com
password=secret
server=xmpp.example.com
port=5222
resource=visualptt
```

`jid`, `password`, and `server` are required. `port` defaults to 5222 and
`resource` defaults to `visualptt`. Use `--config FILE` to select another file.
The password is never passed on a command line or written to diagnostics. The
program warns when the configuration is readable by group or others; a normal
private setting is `chmod 600 ~/.config/visualptt/xmpp.ini`.

## Sending

```sh
./visualptt-xmpp-send --to alice@example.com rec_001.mkv
./visualptt-xmpp-send --to alice@example.com \
  --annotation rec_001.txt rec_001.mkv
```

`--message TEXT` changes the `VisualPTT message` heading, `--ffmpeg PATH`
selects FFmpeg, and `--timeout SECONDS` changes the 30-second operation timeout.
Use `--debug` for XMPP and HTTP diagnostics; authentication secrets are not
included. Run `--help` for all options.

Conversion takes place in a directory made securely under `/tmp`. It is removed
after success or failure. `--keep-converted` retains `output.mp4` and prints its
location, including when a later upload or send step fails.

## Server requirements and limitations

The account must support a TLS-protected normal XMPP client connection and its
domain must advertise a XEP-0363 HTTP File Upload service through XEP-0030
service discovery. Upload slots must provide HTTPS PUT and GET URLs. The sender
announces the MP4 as inline `video/mp4` using XEP-0447 Stateless File Sharing.
It also includes both a plain URL in the body and XEP-0066 out-of-band data for
older clients. A SHA-256 metadata hash lets receiving clients verify the
download before presenting it.

Version 1 sends to one ordinary JID and has no receive, MUC, daemon, OMEMO,
thumbnail, or retry support. H.264 video must already be MP4-compatible:
conversion fails if FFmpeg cannot stream-copy it, and never silently falls back
to an expensive video transcode. A successful queue into the authenticated
XMPP stream does not prove that an offline recipient ultimately received it.

## Testing

`make test-xmpp` runs local CLI, validation, conversion-process, secret-output,
and temporary-cleanup checks without needing an XMPP account. Authentication
failure, discovery failure, successful upload, and successful message delivery
are integration tests: exercise them against a test account/server, since they
depend on server TLS/SASL policy and a real XEP-0363 component. The distinct
exit statuses and `--debug` diagnostics make those stages independently
observable; no password or raw SASL/upload authorization header is logged.
