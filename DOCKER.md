# strfry Docker Setup

This document explains how to run strfry using Docker and Docker Compose.

## Overview

This Docker setup provides a simple way to run a strfry Nostr relay. The configuration includes:

1. A Docker container running the strfry relay
2. Port 7777 exposed for WebSocket connections
3. Persistent storage for the relay database
4. A custom configuration that works well in Docker environments

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/)
- [Docker Compose](https://docs.docker.com/compose/install/)

## Quick Start

1. Clone this repository:
   ```
   git clone https://github.com/hoytech/strfry.git
   cd strfry
   ```

2. Start the strfry relay:
   ```
   docker compose up -d
   ```

3. Check the logs:
   ```
   docker compose logs -f
   ```

The relay will be accessible at `ws://localhost:7777` or `wss://your-domain:7777` if you set up SSL termination with a reverse proxy.

## Configuration

The Docker setup uses a custom configuration file located at `strfry-config/strfry.conf`. You can modify this file to change the relay settings.

Key changes from the default configuration:
- Binds to all interfaces (`0.0.0.0`) instead of just localhost
- Sets up the database path to use the Docker volume
- Enables TCP keepalive for better connection stability
- Configures the `x-real-ip` header for proper IP handling behind proxies
- Explicitly declares support for NIP-12 (tag queries) in the NIP-11 information

## Data Persistence

The relay data is stored in the `strfry-db` directory, which is mounted as a volume in the container. This ensures that your data persists even if the container is removed or rebuilt.

## Stopping the Relay

To stop the relay:
```
docker compose down
```

## Updating

To update to a new version:

1. Pull the latest changes:
   ```
   git pull
   ```

2. Rebuild and restart the container:
   ```
   docker compose down
   docker compose up -d --build
   ```

## Customization

You can customize the relay by editing the `strfry-config/strfry.conf` file. After making changes, restart the container:
```
docker compose restart
```

## Troubleshooting

If you encounter issues:

1. Check the logs:
   ```
   docker compose logs
   ```

2. Ensure the database directory is writable:
   ```
   chmod -R 777 strfry-db
   ```

3. Verify the configuration file is correctly mounted:
   ```
   docker compose exec strfry cat /etc/strfry.conf
   ```
