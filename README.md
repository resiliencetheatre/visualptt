# Visual PTT

Small example programs to send and receive video messages via file system. 
Note that this project contains both written and generated code.

Status of this repository is Work In Progress!

## Reasoning

File-based communication provides a simple, flexible, and transport-independent
way to exchange asynchronous, non-linear messages. A completed message is a
self-contained object that can be queued, copied, delayed, replayed, annotated,
or transported by whatever mechanism is available. The applications at each
end do not need a continuous connection or knowledge of the underlying
transport.

This demonstration code is intended primarily as a real-life test bench for
human communication over geostationary-satellite and deep-space links. Their
latency and intermittent availability make ordinary real-time conversation
impractical, while persistent audiovisual messages allow each participant to
respond in their own time. The same approach is useful when developing
communication systems for other remote or austere environments with limited,
delayed, or unreliable connectivity.

The design can also be used when crossing security-level boundaries in
data-diode-connected environments. Messages can be transferred as discrete
files over an approved one-way path, then inspected, queued, and presented by
an isolated receiver. The surrounding deployment remains responsible for
appropriate validation, filtering, access control, and handling rules at each
security boundary.

## Installation

Install required packages:

```
sudo apt install \
libgtk-3-dev \
libgstreamer1.0-dev \
libgstreamer-plugins-base1.0-dev \
gstreamer1.0-alsa \
gstreamer1.0-plugins-base \
gstreamer1.0-plugins-good \
gstreamer1.0-plugins-bad \
gstreamer1.0-plugins-ugly \
ffmpeg
```

Build and install:

```
git clone https://codeberg.org/resiliencetheatre/visualptt.git
cd visualptt
make
sudo make install
```

The persistent GTK receiver keeps received messages in its list and on disk:

```
visualptt-rx-list /path/to/output
```

Select a timestamp to replay that message. `Auto play` is enabled by default
and plays messages as they arrive; clearing it leaves new messages in the list
for manual playback.

If a matching annotation file exists beside a message (for example,
`rec_20260805_044147.txt` for `rec_20260805_044147.mkv`), its contents are shown
in the text area above `Auto play`. Annotations are optional and may arrive
after the video; the receiver checks for updates while it is running.
Deleting a message also removes matching `.txt` and `.wav` annotation files.

### Annotation watcher

`annotation-watcher` is built and installed with the other Visual PTT
programs. It watches a directory for completed `.mkv` messages, uses FFmpeg to
extract 16 kHz mono WAV audio, and passes that audio to `whisper-cli`. The
resulting `.wav` and `.txt` files are written beside the message and detected
automatically by the receiver.

Run it with the same incoming-message directory used by the receiver:

```
annotation-watcher -d /path/to/incoming
```

Available options are:

```
-d directory     directory to watch (default: samples)
-i seconds       polling interval from 1 to 3600 (default: 2)
-F path          FFmpeg executable (default: /usr/bin/ffmpeg)
-W path          whisper-cli executable (default: /usr/local/bin/whisper-cli)
-h               show help
```

FFmpeg and [whisper.cpp](https://github.com/ggml-org/whisper.cpp) are runtime
requirements for annotation. Installing, building, and configuring
whisper.cpp is outside the scope of the Visual PTT installation.

Whisper.cpp also requires a compatible GGML model. The watcher calls
`whisper-cli` without `-m`, so the CLI's default model must be available as
`models/ggml-base.en.bin` relative to the directory from which the watcher is
started. Alternatively, `-W` can point to a compatible wrapper that supplies
the desired model and then invokes `whisper-cli`.

Follow the [upstream whisper.cpp quick start](https://github.com/ggml-org/whisper.cpp#quick-start)
for current build and model instructions. Its `make base.en` convenience
command downloads the `base.en` model automatically and runs inference after
building; the documented `models/download-ggml-model.sh` command can download
a model separately. Visual PTT and `annotation-watcher` do not download or
install whisper.cpp or its models.

Run the combined GTK receiver and push-to-talk transmitter with an incoming
message directory followed by the destination for completed recordings:

```
visualptt /path/to/incoming /path/to/outgoing
```

The keyboard, PTT event values, hold threshold, and sounds still come from
`pttkey.ini`. The two command-line directories override its message paths.

## Installation

Basic installation notes for Debian 13 host.

```
chown -R $USER:$USER /opt/visualptt
mkdir -p /opt/visualptt/output
cp pttkey.ini /opt/visualptt/
```

Create user service for visualptt-tx:

```
mkdir -p ~/.config/systemd/user
cp systemd/visualptt-tx.service ~/.config/systemd/user/
```

Make sure your user is part of required groups:

```
sudo usermod -aG audio,video,input $USER
```

Enable and start systemd service:

```
systemctl --user daemon-reload
systemctl --user enable --now visualptt-tx.service
systemctl --user status visualptt-tx.service
```

## Configuration

..to be continued..
