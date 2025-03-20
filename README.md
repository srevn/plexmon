# plexmon

A FreeBSD application that monitors filesystem changes in Plex Media Server library directories and triggers partial library scans.

## Features

- Real-time monitoring of Plex library directories using FreeBSD's kqueue
- Automatic detection of Plex libraries and their paths
- Selective partial scans of only changed directories
- Grouping of filesystem events with the same path to prevent scan overload
- Directory structure caching to reduce I/O operations
- Using hash tables for path lookups during comparisons
- Can run as a daemon or in the foreground

## Requirements

- libcurl
- json-c

## Installation

### From Source

```bash
# Install dependencies
pkg install curl json-c

# Clone the repository
git clone https://github.com/srevn/plexmon.git
cd plexmon

# Compile
make

# Install
sudo make install

# Create and edit configuration
sudo cp /usr/local/etc/plexmon.conf.sample /usr/local/etc/plexmon.conf
sudo vi /usr/local/etc/plexmon.conf
```

## Configuration

Edit the configuration file `/usr/local/etc/plexmon.conf`:

```conf
# Plex server URL
plex_url=http://localhost:32400

# Plex authentication token
plex_token=YOUR_PLEX_TOKEN_HERE

# Maximum time to wait for Plex server at startup (in seconds)
startup_timeout=60

# Run in verbose mode
verbose=true

# Run as daemon
daemonize=false

# Log level (info or debug)
log_level=info

# Log file (only used when running as daemon)
log_file=/var/log/plexmon.log
```

### Finding Your Plex Token

1. Sign in to Plex Web App
2. Inspect any request to the Plex server (using browser developer tools)
3. Look for a parameter called `X-Plex-Token`

## Usage

### Running Manually

```bash
# Run in the foreground with verbose output
plexmon -v

# Specify alternate config file
plexmon -c /path/to/config

# Run as daemon
plexmon -d
```