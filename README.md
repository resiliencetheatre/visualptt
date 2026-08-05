# Visual PTT

Small example programs to send and receive video messages via file system. 
Note that this project contains both written and generated code.

Status of this repository is Work In Progress!


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
gstreamer1.0-plugins-ugly
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
