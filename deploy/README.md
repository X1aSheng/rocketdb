# RocketDB Cloud Deployment

This directory contains deployment configuration for running RocketDB
as a network-accessible service on cloud infrastructure.

## Architecture

```
┌──────────────┐     TCP/8080     ┌───────────────────┐
│  Local       │ ◄──────────────► │  RocketDB Server  │
│  Client      │                  │  (Docker/K8s)     │
│  rdb_client  │                  │  rdb_server       │
└──────────────┘                  └───────────────────┘
```

## Quick Start

### 1. Build Docker Image

```bash
docker build -t rocketdb .
```

### 2. Run Locally

```bash
docker run --rm -p 8080:8080 rocketdb
```

### 3. Connect with Client

Build the client on your local machine:

```bash
# Linux/macOS
clang -Isrc -o rocketdb_client deploy/client/rdb_client.c
./rocketdb_client -h <server-ip> -p 8080
```

### 4. Deploy to Kubernetes

```bash
kubectl apply -f k8s/rocketdb.yaml
# Check status
kubectl -n rocketdb get pods
# Port-forward for local testing
kubectl -n rocketdb port-forward svc/rocketdb 8080:8080
```

## Protocol

The server uses a simple text-based protocol over TCP:

| Command | Format | Description |
|---------|--------|-------------|
| SET | `SET key hex_value` | Store a key-value pair |
| GET | `GET key` | Retrieve a value (returns hex) |
| DEL | `DEL key` | Delete a key |
| EXISTS | `EXISTS key` | Check if key exists |
| APPEND | `APPEND timestamp hex_data` | Append TSDB record |
| QUERY | `QUERY from to` | Query TSDB time range |
| STATS | `STATS` | Get engine statistics |
| SPACE | `SPACE` | Get space usage |
| BYE | `BYE` | Disconnect |

Values and data payloads are hex-encoded to avoid binary transport issues.

## Cloud Server Setup

For Ubuntu 26.04 cloud server:

```bash
# Install Docker
apt-get update && apt-get install -y docker.io

# Build and run
docker build -t rocketdb .
docker run -d --restart always -p 8080:8080 --name rocketdb rocketdb

# Test from another terminal
echo "STATS" | nc <server-ip> 8080
```
