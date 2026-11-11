# Multi-node Resource Isolation Test

Scripts to start and stop the pullpiri + TIMPANI based resource-isolation
scenario across master and guest nodes. Running the `run/stop` scripts from the
master PC orchestrates both nodes over SSH.

## Layout

```
test_script/
├── config.env                        # settings (edit here only)
├── run_resource_isolation_flow.sh    # start (SSH orchestration)
├── stop_resource_isolation_flow.sh   # stop
├── preflight/
│   ├── prepare_master.sh             # run directly on the master node
│   └── prepare_guest.sh              # run directly on the guest node
├── lib/utils.sh                      # shared functions (SSH wrappers, etc.)
├── pullpiri/                         # pullpiri install/uninstall scripts, node config
└── timpani/                          # timpani-o/-n install/uninstall scripts, node config
```

## Prerequisites

- `sdv-blueprint` deployed on each node (paths set independently via
  `MASTER_BASE_DIR` / `GUEST_BASE_DIR` in config.env — they may differ)
- master: `docker`, `arduino-cli`, `podman`, `iptables` with the `conntrack` match
- guest: `podman`, `arduino-cli`
- master PC: `ssh`, and `sshpass` (when using password authentication)

## config.env

Edit only the **USER SETTINGS** section at the top.

| Variable | Description |
|----------|-------------|
| `MASTER_HOST` / `PORT` / `USER` / `PASS` | master SSH connection info |
| `GUEST_HOST` / `PORT` / `USER` / `PASS` | guest SSH connection info |
| `GUEST_NODE_IP` | guest internal-network IP. Empty = auto-detect |
| `MASTER_NODE_IP` | master internal-network IP. Empty = auto-detect on master |
| `MASTER_BASE_DIR` / `GUEST_BASE_DIR` | `multi_node` path on each node |

**ADVANCED SETTINGS** (no change needed with defaults): sudo passwords, various
paths, TIMPANI locations, databroker log watch (`DATABROKER_LOG_REGEX`,
`WAIT_LOG_TIMEOUT_SEC`), SSH options (`USE_SSHPASS`, etc.).

> **kuksa-bridge address**: leave `KUKSA_HOST` in
> [../nuc_guest/kuksa-bridge/.env](../nuc_guest/kuksa-bridge/.env) empty.
> `prepare_guest.sh` fills it with the master IP (`MASTER_NODE_IP` or the default
> gateway) right before building the image.

## Scripts

| Script | Runs on | Role |
|--------|---------|------|
| `preflight/prepare_master.sh` | master | udev rules, Arduino upload, serial-bridge image build, databroker pull |
| `preflight/prepare_guest.sh` | guest | Arduino upload, led/kuksa-bridge/stress-ng image build (`KUKSA_HOST` auto-setup) |
| `run_resource_isolation_flow.sh` | master PC | resolve master IP -> start serial-bridge -> install pullpiri -> restart nodeagent -> monitoring/timpani-o -> wait for databroker log -> timpani-n |
| `stop_resource_isolation_flow.sh` | master PC | compose down, stop monitoring, kill timpani, uninstall pullpiri, clean up containers |

## Test Procedure

### 1. Prepare (one-time, run directly on each node)

```bash
# on the master node
./preflight/prepare_master.sh

# on the guest node
./preflight/prepare_guest.sh
```

### 2. Start (master PC)

```bash
cd multi_node/test_script
./run_resource_isolation_flow.sh
```

The script drives both nodes over SSH in order, then waits for Arduino input at **Step 6**.

- **Turn the rotary** on the master -> workload containers are launched on the guest
- Once the script detects the master's databroker log (`api/artifact ... status=200`),
  it proceeds to **Step 7** (guest timpani-n) automatically

### 3. Observe

- **Press the joystick button** on the master to toggle the guest's CPU load (stress-ng)
- Compare the two LED periods in Grafana (`http://<guest-ip>:3000`)
  - TIMPANI LED: stays ~500ms regardless of load
  - Normal LED: delayed beyond 500ms under load

### 4. Stop (master PC)

```bash
./stop_resource_isolation_flow.sh
```

## Notes

- Start/stop scripts add/remove an `iptables` FORWARD ACCEPT rule for guest-to-TIMPANI-O TCP port 7777. Stop before changing node IPs in `config.env`.
- The `node:` field in the workload YAML (guest) must match the guest's `hostname` for scheduling.
- Scripts work regardless of the current working directory (paths are resolved relative to their own location)
